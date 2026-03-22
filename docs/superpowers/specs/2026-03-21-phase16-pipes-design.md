# Phase 16 — Pipes + I/O Redirection Design Spec

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add Unix pipes and input redirection to Aegis so shell pipelines like
`ls /bin | grep sh` and `cat < /etc/motd` work correctly.

**Architecture:** A kernel-side pipe VFS driver backed by a 4 KB ring buffer
with `sched_block`/`sched_wake` for flow control. Three new syscalls
(`pipe2`, `dup`, `dup2`). Shell parser rewritten to support N-stage pipelines
and `<` / `2>&1` redirections. Output redirection (`>file`) deferred to
Phase 19 (ext2 writable FS).

**Tech Stack:** C (kernel pipe driver, syscalls), NASM (no changes), Rust/cap
(no changes), musl-gcc (shell rewrite).

---

## Scope

**In scope:**
- `sys_pipe2` (syscall 293)
- `sys_dup` (syscall 32) and `sys_dup2` (syscall 33)
- Pipe VFS driver: ring buffer, blocking read/write, EOF/EPIPE semantics
- Shell: N-stage pipelines (`|`), input redirection (`<`), `2>&1`
- `PROC_MAX_FDS` bump: 8 → 16 (defined in `kernel/fs/vfs.h`)
- `vfs_ops_t` extended with optional `dup` hook
- `sys_fork` updated to call `dup` hook on inherited fds
- `sys_read` fixed: propagate negative return values; increase kernel buffer cap
- `sched_exit` fixed: close all open fds before marking task zombie

**Out of scope (Phase 19+):**
- Output redirection to files (`>`, `>>`) — requires writable FS
- `O_CLOEXEC` flag on `pipe2`
- Named pipes (FIFOs)
- Signal `SIGPIPE` — Phase 17
- `sys_poll`/`sys_select` on pipe fds — Phase 22

---

## Kernel: Pipe Subsystem

### New files
- `kernel/fs/pipe.h` — `pipe_t` struct, `PIPE_BUF_SIZE`, function declarations
- `kernel/fs/pipe.c` — implementation

### `pipe_t` struct

```c
/* PIPE_BUF_SIZE must be chosen so sizeof(pipe_t) <= 4096 (one kva page).
 * Metadata fields after buf: 5x uint32_t (20 bytes) + 2x pointer (16 bytes)
 * = 36 bytes. So PIPE_BUF_SIZE = 4096 - 36 = 4060. */
#define PIPE_BUF_SIZE 4060

typedef struct {
    uint8_t          buf[PIPE_BUF_SIZE];
    uint32_t         read_pos;
    uint32_t         write_pos;
    uint32_t         count;           /* bytes currently buffered */
    uint32_t         read_refs;       /* number of open read-end fds */
    uint32_t         write_refs;      /* number of open write-end fds */
    aegis_task_t    *reader_waiting;  /* task blocked waiting for data */
    aegis_task_t    *writer_waiting;  /* task blocked waiting for space */
} pipe_t;
/* static_assert(sizeof(pipe_t) <= 4096, "pipe_t must fit in one page"); */
```

`sizeof(pipe_t)` = 4060 + 20 + 16 = 4096 bytes exactly. Allocate via
`kva_alloc_pages(1)`; free via `kva_free_pages(pipe, 1)` in all paths.

### VFS ops

Two separate `vfs_ops_t` singletons. Use **designated initializers** to make
field mapping unambiguous and future-proof against struct reordering:

```c
static const vfs_ops_t g_pipe_read_ops = {
    .read    = pipe_read_fn,
    .write   = NULL,
    .close   = pipe_read_close_fn,
    .readdir = NULL,
    .dup     = pipe_dup_read_fn,
};
static const vfs_ops_t g_pipe_write_ops = {
    .read    = NULL,
    .write   = pipe_write_fn,
    .close   = pipe_write_close_fn,
    .readdir = NULL,
    .dup     = pipe_dup_write_fn,
};
```

All existing ops structs (initrd, console, kbd) must also be updated to use
designated initializers and set `.dup = NULL` explicitly.

**`pipe_read_fn(priv, buf, offset, len)`:**
- `offset` is ignored (pipes have no seek position; the `vfs_file_t.offset`
  field is not updated for pipe fds)
- `buf` is a kernel buffer (`kbuf` inside `sys_read`), not a user pointer —
  plain `memcpy` is correct here

Intended flow with retry-as-loop:
```
loop:
  if count == 0 and write_refs == 0: return 0  /* EOF */
  if count == 0 and write_refs > 0:
      reader_waiting = s_current   /* s_current via sched_current() */
      sched_block()                /* task suspends here; resumes below */
      /* on resume: another task wrote data or closed the write end.
         go back to top of loop — re-check both conditions. */
      goto loop
  /* count > 0: data available */
  n = min(len, count)
  memcpy(buf, &ring[read_pos], n)   /* wrapping handled */
  read_pos = (read_pos + n) % PIPE_BUF_SIZE
  count -= n
  if writer_waiting: sched_wake(writer_waiting); writer_waiting = NULL
  return n
```

EOF-while-blocked path: if the write end is closed while a reader is blocked,
`pipe_write_close_fn` calls `sched_wake(reader_waiting)`. The reader resumes,
retries the loop, sees `count == 0 && write_refs == 0`, and returns 0. This is
the correct EOF signal.

**`pipe_write_fn(priv, buf, len)`:**
- `buf` is a **user virtual address** (from `sys_write`). Use
  `copy_from_user(staging, buf, n)` for SMAP correctness.
- `staging` is a local `char staging[PIPE_BUF_SIZE]` on `pipe_write_fn`'s
  stack frame (4060 bytes). See kernel stack budget note in Constraints.

Intended flow with retry-as-loop:
```
loop:
  if read_refs == 0: return -EPIPE
  if count == PIPE_BUF_SIZE:
      writer_waiting = s_current
      sched_block()
      goto loop
  n = min(len, PIPE_BUF_SIZE - count)
  copy_from_user(staging, buf, n)
  memcpy(&ring[write_pos], staging, n)   /* wrapping handled */
  write_pos = (write_pos + n) % PIPE_BUF_SIZE
  count += n
  if reader_waiting: sched_wake(reader_waiting); reader_waiting = NULL
  return n   /* partial write; sys_write caller must loop if n < len */
```

**`pipe_read_close_fn(priv)`:**
1. Decrement `read_refs`
2. If `writer_waiting != NULL`: `sched_wake(writer_waiting)`, clear it
   (writer will get -EPIPE on next call)
3. If `read_refs == 0` and `write_refs == 0`: `kva_free_pages(pipe, 1)`

**`pipe_write_close_fn(priv)`:**
1. Decrement `write_refs`
2. If `reader_waiting != NULL`: `sched_wake(reader_waiting)`, clear it
   (reader will get EOF on next call via the retry loop)
3. If `read_refs == 0` and `write_refs == 0`: `kva_free_pages(pipe, 1)`

**`pipe_dup_read_fn(priv)` / `pipe_dup_write_fn(priv)`:**
Increment `read_refs` or `write_refs` respectively. Called by `sys_dup`,
`sys_dup2`, and `sys_fork` when duplicating a pipe fd.

### VFS changes: optional `dup` hook

`vfs_ops_t` in `vfs.h` gains one field:

```c
typedef struct {
    vfs_read_fn_t    read;
    vfs_write_fn_t   write;
    vfs_close_fn_t   close;
    vfs_readdir_fn_t readdir;
    vfs_dup_fn_t     dup;   /* NEW: NULL = stateless, no action needed */
} vfs_ops_t;
```

All existing ops structs (initrd, console, kbd) set `.dup = NULL` via
designated initializers.

---

## Kernel: `sys_read` fixes

Two pre-existing limitations in `sys_read` must be fixed as part of this phase:

**Fix 1 — Error propagation.** Current code:
```c
int got = f->ops->read(f->priv, kbuf, f->offset, n);
if (got <= 0) return 0;
```
This converts any negative return (including `-EPIPE` from a future pipe-read
error) into a silent EOF. Fix:
```c
int64_t got = (int64_t)f->ops->read(f->priv, kbuf, f->offset, n);
if (got < 0) return (uint64_t)got;   /* propagate -errno */
if (got == 0) return 0;              /* clean EOF */
```

**Fix 2 — Buffer size cap.** Current code caps `sys_read` at 256 bytes.
Pipe reads need up to `PIPE_BUF_SIZE` bytes per call for reasonable
throughput. Increase:
```c
#define SYS_READ_BUF 4096
char kbuf[SYS_READ_BUF];
uint64_t n = arg3 < SYS_READ_BUF ? arg3 : SYS_READ_BUF;
```
Stack usage increases from 256 to 4096 bytes. See kernel stack budget note.

---

## Kernel: New Syscalls

### `sys_pipe2` — syscall 293

```
int pipe2(int pipefd[2], int flags)
```

1. Validate `pipefd` pointer (2 ints = 8 bytes, user space) via `user_ptr_valid`
2. Find two free slots in `proc->fds[]`; return `-EMFILE` if fewer than 2
3. Allocate `pipe_t` via `kva_alloc_pages(1)`; zero it with `memset`
4. Set `read_refs = 1`, `write_refs = 1`
5. Install read-end `vfs_file_t` at `fds[read_fd]`:
   `ops = &g_pipe_read_ops`, `priv = pipe`, `offset = 0`, `size = 0`
6. Install write-end `vfs_file_t` at `fds[write_fd]`:
   `ops = &g_pipe_write_ops`, `priv = pipe`, `offset = 0`, `size = 0`
7. Copy `[read_fd, write_fd]` to user `pipefd` via `copy_to_user`
8. `flags` accepted but ignored (O_CLOEXEC deferred)
9. Return 0

### `sys_dup` — syscall 32

```
int dup(int oldfd)
```

1. Validate `oldfd` in range and `fds[oldfd].ops != NULL`; return `-EBADF` otherwise
2. Find first free slot `newfd`; return `-EMFILE` if none
3. Copy `fds[oldfd]` to `fds[newfd]` (struct copy by value)
4. If `fds[newfd].ops->dup != NULL`: call `fds[newfd].ops->dup(fds[newfd].priv)`
5. Return `newfd`

### `sys_dup2` — syscall 33

```
int dup2(int oldfd, int newfd)
```

1. Validate `oldfd` in range and open; return `-EBADF` otherwise
2. Validate `newfd` in range (`0 ≤ newfd < PROC_MAX_FDS`); return `-EBADF` otherwise
3. If `oldfd == newfd`: return `newfd` (no-op)
4. If `fds[newfd].ops != NULL`: call `fds[newfd].ops->close(fds[newfd].priv)`,
   zero the slot (`memset(&fds[newfd], 0, sizeof(vfs_file_t))`)
5. Copy `fds[oldfd]` to `fds[newfd]` (struct copy by value)
6. If `fds[newfd].ops->dup != NULL`: call `fds[newfd].ops->dup(fds[newfd].priv)`
7. Return `newfd`

### `sys_fork` update

After the fd table copy-by-value loop, add a second loop to call dup hooks:

```c
/* fds[] was copied by value above (memcpy semantics — no ref bumps yet).
 * Now call dup hooks to reflect that the child holds an additional reference
 * to each fd. This two-pass ordering is required: if dup hooks ran during
 * the copy loop, a failed allocation midway could leave ref counts
 * inconsistent. Copy all first, then bump all. */
for (int i = 0; i < PROC_MAX_FDS; i++) {
    if (child->fds[i].ops && child->fds[i].ops->dup)
        child->fds[i].ops->dup(child->fds[i].priv);
}
```

### `sched_exit` update — close fds on process exit

`sched_exit` currently marks user processes as `TASK_ZOMBIE` without closing
their open fds. This breaks pipes: the write-end of a pipe never calls
`pipe_write_close_fn`, so `write_refs` never reaches zero, so the reader
blocks forever waiting for EOF instead of seeing it.

The fd-close loop must run **before** `vmm_free_user_pml4`. Pipe close ops
access `pipe_t` which lives in kva (kernel virtual space) — safe regardless
of which PML4 is active. However, any future close callback that calls
`copy_to_user` (e.g., a hypothetical Phase 17 signal-fd cleanup) would fault
after the user PML4 is freed. The rule: **fd close loop always runs before
`vmm_free_user_pml4`**.

In `sched_exit`, user-process branch, before `vmm_free_user_pml4`:

```c
/* Close all open fds before freeing user address space.
 * Required for pipe ref count correctness: write-end close triggers
 * sched_wake on any blocked reader, which must happen before the
 * task is marked TASK_ZOMBIE. Also required for any future fd type
 * whose close op accesses user memory (must run before vmm_free_user_pml4). */
aegis_process_t *dying_proc = (aegis_process_t *)dying;
for (int i = 0; i < PROC_MAX_FDS; i++) {
    if (dying_proc->fds[i].ops) {
        dying_proc->fds[i].ops->close(dying_proc->fds[i].priv);
        dying_proc->fds[i].ops = NULL;
    }
}
```

---

## Process: FD Table Bump

`PROC_MAX_FDS` is defined in `kernel/fs/vfs.h` (not `proc.h`):

```c
#define PROC_MAX_FDS 16   /* was 8 */
```

`aegis_process_t.fds[16]` — struct grows by ~256 bytes. Still fits in its kva
allocation. All loops over fds already use `PROC_MAX_FDS`; no other changes
needed.

---

## Shell: Parser Rewrite

### FD budget

The shell parent process temporarily holds all pipe fds during pipeline setup:
fds 0/1/2 (pre-opened) + 2 fds per pipe. With `PROC_MAX_FDS = 16`, the
maximum pipeline depth is `(16 - 3) / 2 = 6` stages.

### New command representation

```c
#define MAX_PIPELINE 6
#define MAX_ARGV     16

typedef struct {
    char *argv[MAX_ARGV];
    char *stdin_file;       /* path for < redirect, or NULL */
    char *stdout_file;      /* path for > redirect (stored, ignored until Phase 19) */
    int   stderr_to_stdout; /* 1 if 2>&1 */
} cmd_t;
```

### Parsing

**`parse_pipeline(line, cmds, max)`**: Split `line` on unquoted `|` characters.
Return count of stages (capped at `MAX_PIPELINE`). Each stage string passed to
`parse_command`.

**`parse_command(segment, cmd)`**: Tokenize on whitespace. For each token:
- If `<`: next token is `cmd->stdin_file`; skip both from argv
- If `>` or `>>`: next token is `cmd->stdout_file` (stored, ignored until
  Phase 19); skip both
- If `2>&1`: set `cmd->stderr_to_stdout = 1`; skip token
- Otherwise: add to `cmd->argv`

### Pipeline execution

```
run_pipeline(cmds, n):
  create n-1 pipes: pipes[0..n-2][2]   (pipes[i][0]=read, pipes[i][1]=write)

  pids[MAX_PIPELINE]
  for i in 0..n-1:
    pid = fork()
    if pid == 0:  // child
      // Set up stdin/stdout redirects via dup2
      if i > 0:   dup2(pipes[i-1][0], STDIN_FILENO)
      if i < n-1: dup2(pipes[i][1], STDOUT_FILENO)
      if stdin_file: fd=open(stdin_file,O_RDONLY); dup2(fd,0); close(fd)
      if stderr_to_stdout: dup2(1, 2)

      // CRITICAL: close all pipe fds in the child after dup2 redirects.
      // If any write-end fd remains open in a child that is reading from the
      // same pipe, that child will never see EOF even after the writer exits —
      // because the write-end ref count stays > 0 (the child itself holds it).
      for j in 0..n-2: close(pipes[j][0]); close(pipes[j][1])

      execve(...)
      _exit(127)
    pids[i] = pid

  // Parent: close all pipe fds after all children are forked.
  // Must happen before waitpid — otherwise the parent holds the write ends
  // open and the last-stage reader never gets EOF.
  for j in 0..n-2: close(pipes[j][0]); close(pipes[j][1])

  // Wait for all children in order
  for i in 0..n-1: waitpid(pids[i], &status, 0)
```

**Builtins** (`cd`, `exit`, `help`): only executed when `n == 1` and
`cmds[0].stdin_file == NULL` and no pipes. A builtin in a pipeline
(e.g., `echo foo | cd`) prints an error and is skipped.

### Shell source size estimate

Current: ~93 lines. After rewrite: ~280 lines. Single file `user/shell/main.c`.

---

## Testing

`make test` boot.txt is unchanged — pipes are exercised only in the interactive
shell, not the boot sequence. Boot test stays green.

Manual smoke tests (run via `make shell`):
```
ls /bin | cat
echo hello | cat
ls /bin | cat | cat
cat < /etc/motd
ls /bin 2>&1 | cat
```

The test suite (`tests/`) gains a new `test_pipe.py` (or extends
`test_shell.py`) that automates these via QEMU serial I/O, analogous to the
Phase 15 shell test.

---

## File Inventory

| Action | File | What changes |
|--------|------|--------------|
| New | `kernel/fs/pipe.h` | `pipe_t`, `PIPE_BUF_SIZE 4060`, function declarations |
| New | `kernel/fs/pipe.c` | Full pipe driver implementation |
| Modify | `kernel/fs/vfs.h` | Add `dup` field to `vfs_ops_t`; `PROC_MAX_FDS` 8 → 16 |
| Modify | `kernel/fs/initrd.c` | Update ops struct to designated initializers + `.dup = NULL` |
| Modify | `kernel/fs/console.c` | Update ops struct to designated initializers + `.dup = NULL` |
| Modify | `kernel/fs/kbd_vfs.c` | Update ops struct to designated initializers + `.dup = NULL` |
| Modify | `kernel/syscall/syscall.c` | `sys_read` fixes; `sys_pipe2`, `sys_dup`, `sys_dup2`; `sys_fork` dup-hook loop |
| Modify | `kernel/sched/sched.c` | `sched_exit`: fd-close loop before `vmm_free_user_pml4` |
| Modify | `Makefile` | Add `kernel/fs/pipe.c` to `FS_SRCS` |
| Modify | `user/shell/main.c` | Full parser rewrite (~280 lines) |
| Modify | `tests/` | New pipe smoke tests |

---

## Constraints and Risks

**Kernel stack budget.** Two large stack buffers exist in the syscall paths:
- `sys_read`: `char kbuf[4096]` — 4 KB
- `pipe_write_fn`: `char staging[PIPE_BUF_SIZE]` (4060 bytes) — ~4 KB

These are in separate syscall paths (`sys_read` vs `sys_write`) and do not
stack on top of each other. The kernel stack is 4 pages (16 KB). Total frame
depth for `sys_write → pipe_write_fn` is approximately 4060 + ~300 bytes of
saved frames, well within budget. **Do not add further large locals to any
function in the `sys_write → pipe_write_fn` call chain without re-auditing
the stack budget.**

**`sched_block` retry pattern.** The `sched_block()` call in `pipe_read_fn`
and `pipe_write_fn` is a mid-function suspend. When the task unblocks,
execution resumes at the statement immediately after `sched_block()` — the
`goto loop` / retry. This is the same pattern used by `sys_waitpid`. The
function does not re-enter from the top; it continues within the same stack
frame. Do not refactor to a callback or coroutine model.

**`sched_block` from VFS context.** Pipe read/write call `sched_block()` from
inside `sys_read`/`sys_write`. This is safe — after unblocking, the ops
function returns a byte count to `sys_read`/`sys_write`, which then copies to
user space and returns via `sysret`. No re-entrancy issue at single-core scope.

**`sched_exit` fd-close ordering.** The fd-close loop runs before
`vmm_free_user_pml4`. This is the required order. Future close callbacks that
touch user memory (Phase 17+) will rely on this invariant. Do not reorder.

**Child pipe fd close is mandatory.** After `dup2` redirects in each pipeline
child, all pipe fds must be explicitly closed before `execve`. If a child
retains the write end of a pipe it is reading from, it will never see EOF even
after the writer exits (write_refs stays > 1 because the reader itself holds
one). This is the most common pipe bug — the pipeline execution pseudocode
makes it explicit.

**Single waiter per pipe end.** `pipe_t` holds one `reader_waiting` and one
`writer_waiting`. Multiple tasks blocking on the same pipe end is not a normal
shell scenario at Phase 16 scope. Known limitation.

**Partial writes.** `pipe_write_fn` may write fewer bytes than requested if the
buffer is nearly full. The `sys_write` caller in musl must loop. This matches
POSIX pipe semantics.

**No `SIGPIPE`.** Writers to a closed-read pipe get `-EPIPE` (errno), not a
signal. musl handles `-EPIPE` gracefully. Full `SIGPIPE` delivery is Phase 17.

**`pipe_write_fn` SMAP.** Data arrives as a user virtual address. The
`copy_from_user` + staging buffer pattern (same as `console_write_fn`)
satisfies the SMAP boundary requirement. A plain `memcpy` from a user pointer
will fault.
