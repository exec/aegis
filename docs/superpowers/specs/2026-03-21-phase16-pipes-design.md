# Phase 16 — Pipes + I/O Redirection Design Spec

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add Unix pipes and input redirection to Aegis so shell pipelines like
`ls /bin | grep sh` and `grep foo < /etc/motd` work correctly.

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

**Out of scope (Phase 19+):**
- Output redirection to files (`>`, `>>`) — requires writable FS
- `O_CLOEXEC` flag on `pipe2`
- Named pipes (FIFOs)
- Signal `SIGPIPE` — Phase 17
- `sys_poll`/`sys_select` on pipe fds — Phase 22

---

## Kernel: Pipe Subsystem

### New files
- `kernel/fs/pipe.h` — `pipe_t` struct, `pipe_alloc`, `pipe_free` declarations
- `kernel/fs/pipe.c` — implementation

### `pipe_t` struct

```c
#define PIPE_BUF_SIZE 4096

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
```

The struct is 4096 + 36 bytes = 4132 bytes. Allocate via `kva_alloc_pages(2)`;
free via `kva_free_pages(pipe, 2)` in all close and free paths. The second page
is mostly unused — acceptable at Phase 16 scope.

### VFS ops

Two separate `vfs_ops_t` singletons — both include the `dup` hook:

```c
static const vfs_ops_t g_pipe_read_ops  = {
    pipe_read_fn, NULL, pipe_read_close_fn, NULL, pipe_dup_read_fn
};
static const vfs_ops_t g_pipe_write_ops = {
    NULL, pipe_write_fn, pipe_write_close_fn, NULL, pipe_dup_write_fn
};
```

(`vfs_ops_t` gains an optional `dup` field — see VFS changes below.)

**`pipe_read_fn(priv, buf, offset, len)`:**
- `offset` is ignored (pipes have no seek position)
1. If `count == 0` and `write_refs == 0`: return 0 (EOF)
2. If `count == 0` and `write_refs > 0`: store `s_current` in `reader_waiting`,
   call `sched_block()`, retry from step 1
3. Copy `min(len, count)` bytes from ring buffer into `buf` (plain kernel-to-kernel
   `memcpy` — `buf` is the kernel buffer inside `sys_read`, not a user pointer)
4. Advance `read_pos`, decrement `count`
5. If `writer_waiting != NULL`: call `sched_wake(writer_waiting)`, clear it
6. Return bytes copied (always > 0 on this path)

**`pipe_write_fn(priv, buf, len)`:**
- `buf` is a user virtual address. Use `copy_from_user(kbuf, buf, n)` to copy
  from user space into a kernel-side staging buffer before writing to the ring.
  This is required for SMAP correctness (same pattern as `console_write_fn`).
1. If `read_refs == 0`: return `-EPIPE`
2. If `count == PIPE_BUF_SIZE`: store `s_current` in `writer_waiting`,
   call `sched_block()`, retry from step 1
3. `n = min(len, PIPE_BUF_SIZE - count)`; `copy_from_user(staging, buf, n)`;
   copy `staging` into ring buffer; advance `write_pos`; increment `count` by `n`
4. If `reader_waiting != NULL`: call `sched_wake(reader_waiting)`, clear it
5. Return `n` (partial write; caller must loop if `n < len`)

**`pipe_read_close_fn(priv)`:**
1. Decrement `read_refs`
2. If `writer_waiting != NULL`: `sched_wake(writer_waiting)`, clear it
   (writer will get -EPIPE on next call)
3. If `read_refs == 0` and `write_refs == 0`: `kva_free_pages(pipe, 2)`

**`pipe_write_close_fn(priv)`:**
1. Decrement `write_refs`
2. If `reader_waiting != NULL`: `sched_wake(reader_waiting)`, clear it
   (reader will get EOF on next call)
3. If `read_refs == 0` and `write_refs == 0`: `kva_free_pages(pipe, 2)`

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

All existing ops structs (initrd, console, kbd) set `.dup = NULL`.

---

## Kernel: `sys_read` fixes

Two pre-existing limitations in `sys_read` must be fixed as part of this phase:

**Fix 1 — Error propagation.** Current code:
```c
int got = f->ops->read(f->priv, kbuf, f->offset, n);
if (got <= 0) return 0;
```
This converts any negative return (including `-EPIPE` from a pipe read) into a
silent EOF. Fix: distinguish negative errors from clean EOF:
```c
int64_t got = (int64_t)f->ops->read(f->priv, kbuf, f->offset, n);
if (got < 0) return (uint64_t)got;   /* propagate -errno */
if (got == 0) return 0;              /* EOF */
```

**Fix 2 — Buffer size cap.** Current code caps `sys_read` at 256 bytes via a
stack buffer `char kbuf[256]`. Pipe reads must be able to drain up to
`PIPE_BUF_SIZE` bytes per call (otherwise musl stdio loops excessively and
performance suffers). Increase the cap:
```c
#define SYS_READ_BUF 4096
char kbuf[SYS_READ_BUF];
uint64_t n = arg3 < SYS_READ_BUF ? arg3 : SYS_READ_BUF;
```
Stack usage increases from 256 to 4096 bytes. The kernel stack is 4 pages
(16 KB); this is within budget.

---

## Kernel: New Syscalls

### `sys_pipe2` — syscall 293

```
int pipe2(int pipefd[2], int flags)
```

1. Validate `pipefd` pointer (2 ints = 8 bytes, user space)
2. Find two free slots in `proc->fds[]`; return `-EMFILE` if fewer than 2
3. Allocate `pipe_t` via `kva_alloc_pages(2)`; zero it
4. Set `read_refs = 1`, `write_refs = 1`
5. Install read-end `vfs_file_t` at `fds[read_fd]`:
   `ops = &g_pipe_read_ops`, `priv = pipe`
6. Install write-end `vfs_file_t` at `fds[write_fd]`:
   `ops = &g_pipe_write_ops`, `priv = pipe`
7. Copy `[read_fd, write_fd]` to user `pipefd` via `copy_to_user`
8. `flags` accepted but ignored (O_CLOEXEC deferred)
9. Return 0

### `sys_dup` — syscall 32

```
int dup(int oldfd)
```

1. Validate `oldfd` in range and `fds[oldfd].ops != NULL`
2. Find first free slot `newfd`; return `-EMFILE` if none
3. Copy `fds[oldfd]` to `fds[newfd]`
4. If `fds[newfd].ops->dup != NULL`: call `fds[newfd].ops->dup(fds[newfd].priv)`
5. Return `newfd`

### `sys_dup2` — syscall 33

```
int dup2(int oldfd, int newfd)
```

1. Validate `oldfd` in range and open
2. Validate `newfd` in range (`0 ≤ newfd < PROC_MAX_FDS`)
3. If `oldfd == newfd`: return `newfd`
4. If `fds[newfd].ops != NULL`: call `fds[newfd].ops->close(fds[newfd].priv)`,
   zero slot
5. Copy `fds[oldfd]` to `fds[newfd]`
6. If `fds[newfd].ops->dup != NULL`: call `fds[newfd].ops->dup(fds[newfd].priv)`
7. Return `newfd`

### `sys_fork` update

After the fd table copy-by-value loop, add:

```c
for (int i = 0; i < PROC_MAX_FDS; i++) {
    if (child->fds[i].ops && child->fds[i].ops->dup)
        child->fds[i].ops->dup(child->fds[i].priv);
}
```

This keeps pipe ref counts correct across fork.

### `sched_exit` update — close fds on process exit

`sched_exit` currently marks user processes as `TASK_ZOMBIE` without closing
their open fds. This is benign today (console and kbd close ops are no-ops),
but breaks pipes: the write-end of a pipe never calls `pipe_write_close_fn`,
so `write_refs` never reaches zero, so the reader blocks forever waiting for
EOF instead of seeing it.

In `sched_exit`, in the user-process branch, before marking `TASK_ZOMBIE`:

```c
/* Close all open fds — required for pipe ref count correctness */
aegis_process_t *dying_proc = (aegis_process_t *)dying;
for (int i = 0; i < PROC_MAX_FDS; i++) {
    if (dying_proc->fds[i].ops) {
        dying_proc->fds[i].ops->close(dying_proc->fds[i].priv);
        dying_proc->fds[i].ops = NULL;
    }
}
```

Without this, `ls /bin | cat` hangs after `ls` exits because the pipe write
end is never released.

---

## Process: FD Table Bump

`PROC_MAX_FDS` is defined in `kernel/fs/vfs.h` (not proc.h):

```c
#define PROC_MAX_FDS 16   /* was 8 */
```

`aegis_process_t.fds[16]` — struct grows by ~256 bytes. Still fits in kva
allocation. All loops over fds use `PROC_MAX_FDS` already; no other changes
required beyond updating this one definition.

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
    char *stdin_file;    /* path for < redirect, or NULL */
    char *stdout_file;   /* path for > redirect (NULL until Phase 19) */
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
  create n-1 pipes: pipes[0..n-2][2]  (read=0, write=1)

  pids[MAX_PIPELINE]
  for i in 0..n-1:
    pid = fork()
    if pid == 0:  // child
      if i > 0:   dup2(pipes[i-1][0], STDIN_FILENO)
      if i < n-1: dup2(pipes[i][1], STDOUT_FILENO)
      if stdin_file: fd=open(stdin_file,O_RDONLY); dup2(fd,0); close(fd)
      if stderr_to_stdout: dup2(1, 2)
      // close all pipe fds in child
      for j in 0..n-2: close(pipes[j][0]); close(pipes[j][1])
      execve(...)
      _exit(127)
    pids[i] = pid

  // parent: close all pipe fds after all children are forked
  for j in 0..n-2: close(pipes[j][0]); close(pipes[j][1])

  // wait for all children
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
grep motd < /etc/motd
ls /bin 2>&1 | cat
```

The test suite (`tests/`) gains a new `test_pipe.py` (or extends
`test_shell.py`) that automates these via QEMU serial I/O, analogous to the
Phase 15 shell test.

---

## File Inventory

| Action | File | What changes |
|--------|------|--------------|
| New | `kernel/fs/pipe.h` | `pipe_t`, `PIPE_BUF_SIZE`, function declarations |
| New | `kernel/fs/pipe.c` | Full pipe driver implementation |
| Modify | `kernel/fs/vfs.h` | Add `dup` field to `vfs_ops_t`; `PROC_MAX_FDS` 8 → 16 |
| Modify | `kernel/syscall/syscall.c` | `sys_read` error propagation + buffer size fix; `sys_pipe2`, `sys_dup`, `sys_dup2`; `sys_fork` dup-hook loop |
| Modify | `kernel/sched/sched.c` | `sched_exit`: close all open fds before marking task zombie |
| Modify | `Makefile` | Add `kernel/fs/pipe.c` to `FS_SRCS` |
| Modify | `user/shell/main.c` | Full parser rewrite (~280 lines) |
| Modify | `tests/` | New pipe smoke tests |

---

## Constraints and Risks

**`sched_block` from VFS context**: Pipe read/write call `sched_block()` from
inside `sys_read`/`sys_write` syscall handlers. This is safe — the syscall
handler returns to `syscall_entry.asm` after `sched_block` unblocks, which
then executes `sysret` back to user space with the correct return value.
No re-entrancy issue at single-core scope.

**Single waiter per pipe end**: `pipe_t` holds one `reader_waiting` and one
`writer_waiting`. If two tasks both block reading the same pipe (not a normal
shell scenario), only the first one is woken. Known limitation for Phase 16.

**Partial writes**: `pipe_write_fn` may copy fewer bytes than requested if the
buffer is nearly full. The caller (sys_write loop or musl stdio) must handle
partial writes. This matches POSIX pipe semantics.

**`sys_read` stack grows to 4 KB**: The `kbuf[4096]` on the kernel stack is
safe — the kernel stack is 4 pages (16 KB). The existing 256-byte cap was
unnecessarily restrictive; 4096 bytes is the right size for pipe throughput.

**No `SIGPIPE`**: Writers to a pipe with no readers get `-EPIPE`, not a signal.
musl handles `-EPIPE` errno gracefully in most cases. Full `SIGPIPE` delivery
is Phase 17.

**`pipe_write_fn` staging buffer**: `pipe_write_fn` uses a local `char staging[PIPE_BUF_SIZE]` on its stack for the `copy_from_user` transfer. At 4096 bytes on a 16 KB kernel stack this is within budget.

**`pipe_write_fn` SMAP**: Data arrives as a user virtual address via `sys_write`.
`pipe_write_fn` must use `copy_from_user` (with `arch_stac`/`arch_clac`) to
copy from user space into the kernel ring buffer — same pattern as
`console_write_fn`. A plain `memcpy` from a user pointer will fault at the
SMAP boundary.
