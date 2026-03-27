# Phase 29: Thread Support (clone + futex)

## Summary

Add POSIX thread support to Aegis via `sys_clone()` with `CLONE_VM` flag
handling, futex wait/wake, shared fd tables, per-thread TLS, and thread
group lifecycle management. Enables musl pthreads and musl-internal DNS
resolution (which uses `clone(CLONE_VM|CLONE_VFORK)` internally).

## Motivation

1. **DNS resolution** — musl's `getaddrinfo()` calls
   `clone(CLONE_VM|CLONE_VFORK|SIGCHLD, stack)` to spawn a helper thread
   for DNS queries. Without thread support, DNS fails and curl needs
   `/etc/hosts` workarounds.

2. **Multithreaded programs** — any program using pthreads (web servers,
   databases, language runtimes) requires `clone()` with shared address
   space, futex for synchronization, and proper thread lifecycle.

3. **musl pthreads** — musl's `pthread_create` uses
   `clone(CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|
   CLONE_SYSVSEM|CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID,
   child_stack, ptid, tls, ctid)`. All these flags must be handled.

## Data Structure Changes

### aegis_task_t (kernel/sched/sched.h)

Add two fields:

```c
uint64_t  fs_base;          /* moved FROM aegis_process_t — per-thread TLS */
uint64_t  clear_child_tid;  /* user VA: write 0 + futex_wake on thread exit */
```

`fs_base` moves from `aegis_process_t` to `aegis_task_t` because each
thread in a process needs its own TLS pointer. All existing code that
reads/writes `proc->fs_base` must change to `task->fs_base`.

### aegis_process_t (kernel/proc/proc.h)

Changes:

```c
/* Remove: vfs_file_t fds[PROC_MAX_FDS]; */
/* Add: */
fd_table_t   *fd_table;       /* shared, refcounted fd table */
uint32_t      tgid;           /* thread group ID (= leader's PID) */
uint32_t      thread_count;   /* number of live threads in group */
```

- `tgid` equals the PID of the thread group leader. For the leader
  itself, `tgid == pid`. For spawned threads, `tgid == leader->pid`.
- `thread_count` starts at 1 (the leader). Incremented on clone,
  decremented on thread exit. When it hits 0, full process teardown.
- `fd_table` replaces the inline `fds[]` array. All threads in a group
  share the same `fd_table` pointer when `CLONE_FILES` is set.

### fd_table_t (new, kernel/fs/fd_table.h)

```c
typedef struct {
    vfs_file_t fds[PROC_MAX_FDS];
    uint32_t   refcount;
} fd_table_t;
```

Allocated via `kva_alloc_pages(1)` (4KB is enough for 32 fds * 64 bytes
+ refcount). Reference counted:
- `fd_table_alloc()` — allocate + zero + refcount=1
- `fd_table_ref(t)` — increment refcount
- `fd_table_unref(t)` — decrement; free if 0
- `fd_table_copy(t)` — deep copy (for fork without CLONE_FILES)

All existing `proc->fds[fd]` references change to `proc->fd_table->fds[fd]`.

## sys_clone Implementation

### Syscall Signature

```
sys_clone(flags, child_stack, ptid, ctid, tls)
  x86-64: syscall 56 (rdi=flags, rsi=child_stack, rdx=ptid, r10=ctid, r8=tls)
```

Register mapping note: Linux clone on x86-64 uses a non-standard
argument order. musl's `__clone` wrapper shuffles registers before the
`syscall` instruction. The kernel receives:
- arg1 (rdi) = flags
- arg2 (rsi) = child_stack (user stack pointer for the new thread)
- arg3 (rdx) = ptid (parent tid pointer)
- arg4 (r10) = ctid (child tid pointer)
- arg5 (r8)  = tls (new FS.base value)

### Flag Handling

| Flag | Value | Action |
|------|-------|--------|
| `CLONE_VM` (0x100) | Share PML4 — no `vmm_copy_user_pages` |
| `CLONE_FILES` (0x400) | Share fd_table — `fd_table_ref()` instead of copy |
| `CLONE_SIGHAND` (0x800) | Share sigactions — pointer to leader's array |
| `CLONE_THREAD` (0x10000) | Same tgid, increment thread_count |
| `CLONE_PARENT_SETTID` (0x100000) | Write child TID to `*ptid` in parent's address space |
| `CLONE_CHILD_SETTID` (0x01000000) | Write child TID to `*ctid` in child's address space |
| `CLONE_CHILD_CLEARTID` (0x200000) | Store ctid in `clear_child_tid`; 0+wake on exit |
| `CLONE_SETTLS` (0x80000) | Set child's `fs_base` from `tls` argument |
| `CLONE_VFORK` (0x4000) | Parent blocks until child exits or execs |
| `CLONE_FS` (0x200) | Accepted, no-op (we share cwd via tgid) |
| `CLONE_SYSVSEM` (0x40000) | Accepted, no-op (no SysV semaphores) |

When no `CLONE_VM` flag is present, `sys_clone` behaves identically to
the current `sys_fork` (full address space copy).

### Capability Gate

`CLONE_VM` requires `CAP_KIND_THREAD_CREATE` (new, value 9). This
capability is granted in the execve baseline (all processes can create
threads by default). Future work: vigil service configs can revoke it
via `-THREAD_CREATE` syntax for sandboxed services.

### Child Stack Setup

When `CLONE_VM` is set, the child uses `child_stack` (arg2) as its user
stack pointer. musl allocates this stack via `mmap(MAP_ANONYMOUS)` before
calling clone. The kernel builds the child's initial kernel stack frame
the same way as fork, but the user RSP in the iretq frame points to
`child_stack` instead of the parent's RSP.

When `CLONE_VM` is NOT set (fork), `child_stack` is ignored if 0 (child
inherits parent's stack, which is safe because the address space is
copied).

### Thread Identification

- Each thread gets a unique TID from the global PID counter (same
  counter as `pid` — Linux does this too).
- `sys_getpid()` returns `proc->tgid` (not `proc->pid`) so all threads
  in a group report the same PID.
- `sys_gettid()` (syscall 186) returns `proc->pid` (the actual per-thread ID).
- For the thread group leader, `tgid == pid`.

## Futex

### Design

A global hash table of 32 buckets, each containing a linked list of
waiting tasks. Key: virtual address (as `uint64_t`). Since threads share
address space (`CLONE_VM`), the same VA maps to the same physical page
for all threads in a group.

### Data Structures (kernel/syscall/futex.c)

```c
#define FUTEX_BUCKETS 32

typedef struct futex_waiter {
    aegis_task_t          *task;
    uint64_t               addr;    /* user VA being waited on */
    struct futex_waiter   *next;
} futex_waiter_t;

static futex_waiter_t *s_buckets[FUTEX_BUCKETS];
```

Waiters are allocated from a small static pool (64 slots — more than
enough for Phase 29 scope).

### Operations

**`FUTEX_WAIT(addr, expected, timeout)`** (op=0):
1. Read `*addr` from user space (via `copy_from_user`)
2. If `*addr != expected`, return `-EAGAIN`
3. Allocate waiter slot, add to bucket `hash(addr) % 32`
4. `sched_block()` — task sleeps until woken
5. On wake: remove from bucket, return 0

**`FUTEX_WAKE(addr, count)`** (op=1):
1. Walk bucket `hash(addr) % 32`
2. For each waiter with matching addr, `sched_wake(task)`, remove from list
3. Stop after waking `count` tasks
4. Return number of tasks woken

**`FUTEX_PRIVATE_FLAG`** (0x80): stripped and ignored (all futexes are
process-private in Aegis — no shared memory between processes yet).

### Syscall Number

x86-64: syscall 202 (`SYS_futex`)
ARM64: syscall 98 → translates to 202

## Thread Exit

### Single Thread Exit

When a thread calls `_exit()` (not the group leader):

1. If `clear_child_tid` is set:
   a. Write 0 to `*clear_child_tid` via `vmm_write_user_bytes`
   b. `futex_wake(clear_child_tid, 1)` — wakes `pthread_join` waiter
2. Decrement `thread_count` in the process
3. If `thread_count == 0`: full process teardown (free PML4, fd_table,
   caps, signal state)
4. If `thread_count > 0`: free only this thread's kernel stack, mark
   zombie for deferred cleanup (existing `sched_exit` pattern)

### exit_group (syscall 231)

Kills all threads in the thread group:

1. Walk the scheduler's task list
2. For each task with same `tgid`: set state to `TASK_ZOMBIE`, decrement
   `thread_count`
3. The calling thread does its own exit last (same as single thread exit)
4. This triggers full process teardown since `thread_count` reaches 0

### Process Teardown (thread_count == 0)

Same as current `sched_exit` but with fd_table changes:
- `fd_table_unref(proc->fd_table)` instead of closing fds inline
- Free PML4 only if this process owns it (leader or last thread)

## fs_base Migration

### What Changes

`fs_base` moves from `aegis_process_t` to `aegis_task_t`. Every
location that references `proc->fs_base` or
`((aegis_process_t *)task)->fs_base` must change to `task->fs_base`.

### Affected Code

1. `sys_arch_prctl` — writes `task->fs_base` instead of `proc->fs_base`
2. `sys_fork` / `sys_clone` — copies from parent task's `fs_base`
3. `sched_tick` — already reads from process; change to task
4. `sched_block` — same
5. `sched_yield_to_next` — same
6. `proc_spawn_init` — sets initial `fs_base = 0` on task, not process

## Capability

```c
#define CAP_KIND_THREAD_CREATE 9u
```

- Checked in `sys_clone` when `flags & CLONE_VM`
- Granted in execve baseline (`sys_execve` in `sys_process.c`)
- Future: vigil service configs can revoke via `-THREAD_CREATE` syntax
  (not implemented in Phase 29; noted in CLAUDE.md forward constraints)

## Syscall Table Changes

| Syscall | Number (x86-64) | Number (ARM64) | Action |
|---------|-----------------|----------------|--------|
| `clone` | 56 | 220 | New full implementation |
| `futex` | 202 | 98 | New (WAIT + WAKE only) |
| `gettid` | 186 | 178 | New (return task's pid) |
| `set_tid_address` | 218 | 96→218 | Fix stub (store pointer, return tid) |
| `getpid` | 39 | 172→39 | Return `tgid` instead of `pid` |
| `exit_group` | 231 | 94→231 | Kill all threads in group |

## Testing

### test_threads.py

Boot with vigil + NVMe disk. Run `thread_test` binary:

1. Creates a pthread, joins it, prints "THREAD OK"
2. Calls `getaddrinfo("10.0.2.3", ...)` to verify DNS works via clone
3. Prints "DNS OK" if resolution succeeds

Expected serial output includes both "THREAD OK" and "DNS OK".

### user/thread_test/main.c

Simple musl program:

```c
#include <pthread.h>
#include <stdio.h>
#include <netdb.h>

void *thread_fn(void *arg) {
    int *val = (int *)arg;
    *val = 42;
    return NULL;
}

int main(void) {
    /* Test 1: pthread create + join */
    int result = 0;
    pthread_t t;
    pthread_create(&t, NULL, thread_fn, &result);
    pthread_join(t, NULL);
    if (result == 42)
        printf("THREAD OK\n");

    /* Test 2: DNS via getaddrinfo (uses clone internally) */
    struct addrinfo *res;
    if (getaddrinfo("10.0.2.3", "53", NULL, &res) == 0) {
        printf("DNS OK\n");
        freeaddrinfo(res);
    }
    return 0;
}
```

### Regression

`make test` must remain GREEN. All existing tests (boot, pipe, signal,
socket, vigil, etc.) must pass unchanged.

## Files Modified

| File | Change |
|------|--------|
| `kernel/sched/sched.h` | Add `fs_base`, `clear_child_tid` to `aegis_task_t` |
| `kernel/proc/proc.h` | Add `tgid`, `thread_count`, `fd_table*`; remove `fds[]`, `fs_base` |
| `kernel/fs/fd_table.h` (new) | `fd_table_t` with refcount |
| `kernel/fs/fd_table.c` (new) | alloc/ref/unref/copy |
| `kernel/syscall/sys_process.c` | `sys_clone` (new), update `sys_fork`, fix `set_tid_address`, `getpid` returns tgid |
| `kernel/syscall/futex.c` (new) | futex_wait, futex_wake |
| `kernel/syscall/syscall.c` | Add clone/futex/gettid dispatch, ARM64 translation |
| `kernel/syscall/sys_impl.h` | New function declarations |
| `kernel/sched/sched.c` | fs_base from task not process; thread exit cleanup |
| `kernel/arch/x86_64/pit.c` | No change (g_poll_waiter already present) |
| `kernel/cap/cap.h` | Add `CAP_KIND_THREAD_CREATE = 9` |
| `kernel/cap/src/lib.rs` | Add `CAP_KIND_THREAD_CREATE` constant |
| `tests/test_threads.py` (new) | Thread + DNS integration test |
| `user/thread_test/` (new) | Simple pthread test program |

## Forward Constraints

- **No `pthread_cancel`** — musl's `pthread_cancel` requires signal
  delivery to a specific thread (via `tgkill`). Not implemented in
  Phase 29. Programs that rely on cancellation will hang.

- **No per-thread signal delivery** — signals go to the process (any
  thread). `tgkill(tgid, tid, sig)` for targeted delivery is Phase 32
  work.

- **fd_table refcount is not atomic** — safe for single-core Phase 29.
  SMP requires atomic operations.

- **Futex pool is static (64 slots)** — sufficient for typical pthread
  usage. Dynamic allocation deferred.

- **`-THREAD_CREATE` vigil syntax** — design intent documented but not
  implemented. Vigil service config parsing unchanged in Phase 29.

- **Shared sigactions** — when `CLONE_SIGHAND` is set, threads should
  share the signal handler table. Phase 29 implements this by having
  threads point to the leader's `sigactions[]` array. Modifying
  `sigaction()` in one thread affects all threads in the group.
