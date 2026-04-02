# Phase 29: Thread Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add POSIX thread support via `sys_clone(CLONE_VM)`, futex, shared fd tables, per-thread TLS, enabling musl pthreads and DNS resolution.

**Architecture:** Threads are lightweight tasks sharing their parent's PML4, fd table, signal handlers, and caps. New `fd_table_t` with refcounting replaces inline `fds[]`. `fs_base` moves from process to task for per-thread TLS. Futex provides wait/wake synchronization keyed by virtual address.

**Tech Stack:** C kernel code, Rust capability constant, musl pthreads for testing.

**Spec:** `docs/superpowers/specs/2026-03-27-phase29-threads-design.md`

---

## Task 1: Move `fs_base` from `aegis_process_t` to `aegis_task_t`

This is a pure refactor with no behavior change. Every thread needs its own TLS pointer. Move the field and update all references. Existing tests validate no regression.

**Files:**
- Modify: `kernel/sched/sched.h:11-21`
- Modify: `kernel/proc/proc.h:22-44`
- Modify: `kernel/sched/sched.c` (9 locations)
- Modify: `kernel/syscall/sys_process.c:91-94,170-173,228`
- Modify: `kernel/proc/proc.c:321`

- [ ] **Step 1: Add `fs_base` to `aegis_task_t`**

In `kernel/sched/sched.h`, add `fs_base` after the `waiting_for` field:

```c
typedef struct aegis_task_t {
    uint64_t             sp;
    uint8_t             *stack_base;
    uint64_t             kernel_stack_top;
    uint32_t             tid;
    uint8_t              is_user;
    uint64_t             stack_pages;
    uint32_t             state;
    uint32_t             waiting_for;
    uint64_t             fs_base;          /* per-thread TLS base (IA32_FS_BASE / TPIDR_EL0) */
    uint64_t             clear_child_tid;  /* user VA: write 0 + futex_wake on exit */
    struct aegis_task_t *next;
} aegis_task_t;
```

Also add `clear_child_tid` now (used in later tasks).

- [ ] **Step 2: Remove `fs_base` from `aegis_process_t`**

In `kernel/proc/proc.h`, remove line 30 (`uint64_t fs_base;`).

- [ ] **Step 3: Update all `sched.c` references**

In `kernel/sched/sched.c`, change every `((aegis_process_t *)s_current)->fs_base` and `p->fs_base` to use the task directly. There are 9 locations. The pattern is:

Replace:
```c
arch_set_fs_base(((aegis_process_t *)s_current)->fs_base);
```
With:
```c
arch_set_fs_base(s_current->fs_base);
```

And replace:
```c
aegis_process_t *p = (aegis_process_t *)s_current;
arch_set_fs_base(p->fs_base);
```
With:
```c
arch_set_fs_base(s_current->fs_base);
```

All 9 locations (lines 228, 306, 326, 361, 372, 396, 416, 483, 519).

- [ ] **Step 4: Update `sys_arch_prctl` in `sys_process.c`**

Change the write from `proc->fs_base = arg2` to `proc->task.fs_base = arg2` (or `sched_current()->fs_base = arg2`).

- [ ] **Step 5: Update `sys_fork` in `sys_process.c`**

Line 173: change `child->fs_base = parent->fs_base` to `child->task.fs_base = parent->task.fs_base`. Same for the ARM64 path (line 170): `child->task.fs_base = tpidr`.

- [ ] **Step 6: Update `proc_spawn` in `proc.c`**

Line 321: change `proc->fs_base = 0` to `proc->task.fs_base = 0`.

- [ ] **Step 7: Build and run `make test`**

```bash
make test
```

Expected: PASS. This is a pure refactor — no behavior change.

- [ ] **Step 8: Commit**

```bash
git add kernel/sched/sched.h kernel/proc/proc.h kernel/sched/sched.c \
        kernel/syscall/sys_process.c kernel/proc/proc.c
git commit -m "refactor: move fs_base from aegis_process_t to aegis_task_t

Per-thread TLS requires each task to have its own fs_base. Move the
field and update all 9 sched.c references + sys_arch_prctl + sys_fork
+ proc_spawn. Pure refactor, no behavior change."
```

---

## Task 2: Introduce `fd_table_t` with reference counting

Replace the inline `fds[PROC_MAX_FDS]` in `aegis_process_t` with a pointer to a shared, refcounted `fd_table_t`. This task creates the new type and migrates all `proc->fds[fd]` references to `proc->fd_table->fds[fd]`.

**Files:**
- Create: `kernel/fs/fd_table.h`
- Create: `kernel/fs/fd_table.c`
- Modify: `kernel/proc/proc.h:25`
- Modify: `kernel/syscall/sys_process.c` (fork fd copy, execve fd close)
- Modify: `kernel/sched/sched.c:157-162` (sched_exit fd close)
- Modify: `kernel/proc/proc.c` (proc_spawn fd init)
- Modify: ~81 `proc->fds[` references across all files
- Modify: `Makefile:139-147` (add fd_table.c to build)

- [ ] **Step 1: Create `kernel/fs/fd_table.h`**

```c
/* kernel/fs/fd_table.h — shared, refcounted file descriptor table */
#ifndef FD_TABLE_H
#define FD_TABLE_H

#include "vfs.h"

typedef struct {
    vfs_file_t fds[PROC_MAX_FDS];
    uint32_t   refcount;
} fd_table_t;

/* Allocate a new fd_table with refcount=1, all fds zeroed. */
fd_table_t *fd_table_alloc(void);

/* Increment refcount (for CLONE_FILES sharing). */
void fd_table_ref(fd_table_t *t);

/* Decrement refcount; free (kva_free_pages) when it reaches 0.
 * Calls ops->close on every open fd before freeing. */
void fd_table_unref(fd_table_t *t);

/* Deep-copy: allocate new fd_table, copy all fds, call dup hooks.
 * Returns new table with refcount=1, or NULL on OOM. */
fd_table_t *fd_table_copy(fd_table_t *src);

#endif /* FD_TABLE_H */
```

- [ ] **Step 2: Create `kernel/fs/fd_table.c`**

```c
/* kernel/fs/fd_table.c — fd_table_t lifecycle */
#include "fd_table.h"
#include "kva.h"

fd_table_t *fd_table_alloc(void)
{
    fd_table_t *t = (fd_table_t *)kva_alloc_pages(1);
    if (!t) return (fd_table_t *)0;
    uint32_t i;
    for (i = 0; i < PROC_MAX_FDS; i++)
        t->fds[i].ops = (const vfs_ops_t *)0;
    t->refcount = 1;
    return t;
}

void fd_table_ref(fd_table_t *t)
{
    if (t) t->refcount++;
}

void fd_table_unref(fd_table_t *t)
{
    if (!t) return;
    if (t->refcount > 1) { t->refcount--; return; }
    /* Last reference — close all fds and free. */
    uint32_t i;
    for (i = 0; i < PROC_MAX_FDS; i++) {
        if (t->fds[i].ops && t->fds[i].ops->close) {
            t->fds[i].ops->close(t->fds[i].priv);
            t->fds[i].ops = (const vfs_ops_t *)0;
        }
    }
    kva_free_pages(t, 1);
}

fd_table_t *fd_table_copy(fd_table_t *src)
{
    if (!src) return (fd_table_t *)0;
    fd_table_t *dst = (fd_table_t *)kva_alloc_pages(1);
    if (!dst) return (fd_table_t *)0;
    uint32_t i;
    for (i = 0; i < PROC_MAX_FDS; i++)
        dst->fds[i] = src->fds[i];
    /* Bump dup hooks after full copy (two-pass for consistency). */
    for (i = 0; i < PROC_MAX_FDS; i++) {
        if (dst->fds[i].ops && dst->fds[i].ops->dup)
            dst->fds[i].ops->dup(dst->fds[i].priv);
    }
    dst->refcount = 1;
    return dst;
}
```

- [ ] **Step 3: Update `aegis_process_t` in `proc.h`**

Replace line 25 (`vfs_file_t fds[PROC_MAX_FDS];`) with:

```c
    fd_table_t   *fd_table;                /* shared, refcounted fd table */
```

Add `#include "fd_table.h"` at the top of proc.h (or forward-declare).

- [ ] **Step 4: Mechanical rename — all `proc->fds[` → `proc->fd_table->fds[`**

This is ~81 references across these files:
- `kernel/syscall/sys_io.c` (sys_write, sys_writev, sys_read)
- `kernel/syscall/sys_file.c` (sys_open, sys_close, sys_dup, sys_dup2, sys_fcntl, sys_fstat, sys_lseek, sys_getdents64)
- `kernel/syscall/sys_process.c` (sys_fork fd copy, sys_execve fd close)
- `kernel/syscall/sys_socket.c` (sock_open_fd, sock_id_from_fd)
- `kernel/net/socket.c` (sock_open_fd)
- `kernel/net/epoll.c`
- `kernel/proc/proc.c` (proc_spawn fd init)
- `kernel/sched/sched.c` (sched_exit fd close)

Use `replace_all` for each file: `proc->fds[` → `proc->fd_table->fds[`. Then manually fix non-`proc->` patterns (e.g. `child->fds[`, `dying->fds[`, `parent->fds[`).

- [ ] **Step 5: Update `sys_fork` fd copy logic**

In `sys_process.c`, replace the manual fd copy loop (lines 140-151) with:

```c
    child->fd_table = fd_table_copy(parent->fd_table);
    if (!child->fd_table) {
        kva_free_pages(child, 1);
        return (uint64_t)-(int64_t)12;
    }
```

- [ ] **Step 6: Update `sched_exit` fd close logic**

In `sched.c`, replace the fd close loop (lines 157-162) with:

```c
    fd_table_unref(dying->fd_table);
    dying->fd_table = (fd_table_t *)0;
```

- [ ] **Step 7: Update `proc_spawn` fd init**

In `proc.c`, where fds are initialized (around line 306-312), replace with:

```c
    proc->fd_table = fd_table_alloc();
    /* ... then open fd 0/1/2 on proc->fd_table->fds[] ... */
```

- [ ] **Step 8: Add `fd_table.c` to Makefile**

In `Makefile` around line 127 (FS_SRCS), add `kernel/fs/fd_table.c`.

- [ ] **Step 9: Build and run `make test`**

```bash
make test
```

Expected: PASS. All fd operations now go through fd_table indirection.

- [ ] **Step 10: Commit**

```bash
git add kernel/fs/fd_table.h kernel/fs/fd_table.c kernel/proc/proc.h \
        kernel/syscall/sys_*.c kernel/net/socket.c kernel/net/epoll.c \
        kernel/proc/proc.c kernel/sched/sched.c Makefile
git commit -m "refactor: introduce fd_table_t with refcounting

Replace inline fds[PROC_MAX_FDS] in aegis_process_t with a pointer to
a heap-allocated, refcounted fd_table_t. Prepares for CLONE_FILES
sharing. All ~81 proc->fds[] references updated. Pure refactor."
```

---

## Task 3: Add `tgid`, `thread_count`, and `CAP_KIND_THREAD_CREATE`

Add thread group fields to `aegis_process_t`, the new capability constant, and update `sys_getpid` to return `tgid`. Also fix `sys_set_tid_address` and add `sys_gettid`.

**Files:**
- Modify: `kernel/proc/proc.h`
- Modify: `kernel/cap/cap.h`
- Modify: `kernel/syscall/sys_process.c`
- Modify: `kernel/syscall/syscall.c`
- Modify: `kernel/syscall/sys_impl.h`
- Modify: `kernel/proc/proc.c`

- [ ] **Step 1: Add fields to `aegis_process_t`**

In `kernel/proc/proc.h`, add after `pid`:

```c
    uint32_t      tgid;                    /* thread group ID (= leader's PID) */
    uint32_t      thread_count;            /* live threads in this group */
```

- [ ] **Step 2: Add `CAP_KIND_THREAD_CREATE` to `cap.h`**

In `kernel/cap/cap.h`, add after `CAP_KIND_NET_ADMIN`:

```c
#define CAP_KIND_THREAD_CREATE 9u  /* may call clone(CLONE_VM) */
```

- [ ] **Step 3: Initialize `tgid` and `thread_count` in `proc_spawn` and `sys_fork`**

In `kernel/proc/proc.c` proc_spawn, after setting `proc->pid`:
```c
    proc->tgid = proc->pid;
    proc->thread_count = 1;
```

In `kernel/syscall/sys_process.c` sys_fork, after setting `child->pid`:
```c
    child->tgid = child->pid;
    child->thread_count = 1;
```

- [ ] **Step 4: Update `sys_getpid` to return `tgid`**

```c
uint64_t
sys_getpid(void)
{
    aegis_task_t *task = sched_current();
    if (!task->is_user) return 0;
    return (uint64_t)((aegis_process_t *)task)->tgid;
}
```

- [ ] **Step 5: Add `sys_gettid`**

```c
uint64_t
sys_gettid(void)
{
    aegis_task_t *task = sched_current();
    if (!task->is_user) return 0;
    return (uint64_t)((aegis_process_t *)task)->pid;
}
```

Add declaration to `sys_impl.h` and dispatch entry in `syscall.c`:
```c
case 186: return sys_gettid();
```

- [ ] **Step 6: Fix `sys_set_tid_address`**

```c
uint64_t
sys_set_tid_address(uint64_t arg1)
{
    aegis_task_t *task = sched_current();
    task->clear_child_tid = arg1;
    if (!task->is_user) return 1;
    return (uint64_t)((aegis_process_t *)task)->pid;
}
```

- [ ] **Step 7: Grant `CAP_KIND_THREAD_CREATE` in execve baseline**

In `sys_process.c`, after the `NET_SOCKET` grant (line 570):
```c
    cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_THREAD_CREATE, CAP_RIGHTS_READ);
```

- [ ] **Step 8: Build and run `make test`**

Expected: PASS. `getpid` now returns tgid (which equals pid for non-threaded processes — no behavior change).

- [ ] **Step 9: Commit**

```bash
git add kernel/proc/proc.h kernel/cap/cap.h kernel/syscall/sys_process.c \
        kernel/syscall/syscall.c kernel/syscall/sys_impl.h kernel/proc/proc.c
git commit -m "feat: add tgid, thread_count, CAP_KIND_THREAD_CREATE

getpid returns tgid (= pid for single-threaded). set_tid_address stores
clear_child_tid pointer. gettid (syscall 186) returns per-thread ID.
THREAD_CREATE cap granted in execve baseline."
```

---

## Task 4: Implement futex (WAIT + WAKE)

Simple hash table of wait queues. Static pool of 64 waiter slots.

**Files:**
- Create: `kernel/syscall/futex.h`
- Create: `kernel/syscall/futex.c`
- Modify: `kernel/syscall/syscall.c`
- Modify: `kernel/syscall/sys_impl.h`
- Modify: `Makefile`

- [ ] **Step 1: Create `kernel/syscall/futex.h`**

```c
/* kernel/syscall/futex.h — futex wait/wake for thread synchronization */
#ifndef FUTEX_H
#define FUTEX_H

#include <stdint.h>

#define FUTEX_WAIT         0
#define FUTEX_WAKE         1
#define FUTEX_PRIVATE_FLAG 128

/* sys_futex — main entry point. op is FUTEX_WAIT or FUTEX_WAKE.
 * addr: user virtual address of the futex word.
 * val:  expected value (WAIT) or max wakeup count (WAKE). */
uint64_t sys_futex(uint64_t addr, uint64_t op, uint64_t val,
                   uint64_t timeout, uint64_t addr2, uint64_t val3);

/* futex_wake_addr — wake waiters on addr. Called from thread exit
 * (clear_child_tid path) without going through the syscall layer. */
int futex_wake_addr(uint64_t addr, uint32_t count);

#endif /* FUTEX_H */
```

- [ ] **Step 2: Create `kernel/syscall/futex.c`**

```c
/* kernel/syscall/futex.c — futex WAIT/WAKE implementation */
#include "futex.h"
#include "sched.h"
#include "../mm/uaccess.h"

#define FUTEX_BUCKETS   32
#define FUTEX_MAX_WAIT  64

typedef struct {
    aegis_task_t *task;
    uint64_t      addr;
    uint8_t       in_use;
} futex_waiter_t;

static futex_waiter_t s_pool[FUTEX_MAX_WAIT];
static futex_waiter_t *s_buckets[FUTEX_BUCKETS];  /* head of each chain */

/* Each waiter has a 'next_in_bucket' index (0 = end of chain).
 * We store the chain via indices into s_pool. */
static uint8_t s_next[FUTEX_MAX_WAIT]; /* next index+1 in bucket chain; 0=end */

static uint32_t bucket_idx(uint64_t addr) { return (uint32_t)(addr >> 2) % FUTEX_BUCKETS; }

static futex_waiter_t *pool_alloc(void)
{
    uint32_t i;
    for (i = 0; i < FUTEX_MAX_WAIT; i++) {
        if (!s_pool[i].in_use) {
            s_pool[i].in_use = 1;
            s_next[i] = 0;
            return &s_pool[i];
        }
    }
    return (futex_waiter_t *)0;
}

int futex_wake_addr(uint64_t addr, uint32_t count)
{
    uint32_t b = bucket_idx(addr);
    int woken = 0;
    uint32_t i;
    for (i = 0; i < FUTEX_MAX_WAIT && (uint32_t)woken < count; i++) {
        if (s_pool[i].in_use && s_pool[i].addr == addr) {
            sched_wake(s_pool[i].task);
            s_pool[i].in_use = 0;
            s_pool[i].task = (aegis_task_t *)0;
            woken++;
        }
    }
    (void)b;
    return woken;
}

uint64_t sys_futex(uint64_t addr, uint64_t op, uint64_t val,
                   uint64_t timeout, uint64_t addr2, uint64_t val3)
{
    (void)timeout; (void)addr2; (void)val3;
    uint32_t cmd = (uint32_t)op & ~(uint32_t)FUTEX_PRIVATE_FLAG;

    if (!user_ptr_valid(addr, sizeof(uint32_t)))
        return (uint64_t)-(int64_t)14; /* EFAULT */

    if (cmd == FUTEX_WAIT) {
        uint32_t uval;
        copy_from_user(&uval, (const void *)(uintptr_t)addr, sizeof(uint32_t));
        if (uval != (uint32_t)val)
            return (uint64_t)-(int64_t)11; /* EAGAIN */
        futex_waiter_t *w = pool_alloc();
        if (!w) return (uint64_t)-(int64_t)12; /* ENOMEM */
        w->task = sched_current();
        w->addr = addr;
        sched_block();
        /* Woken — slot already freed by futex_wake_addr */
        return 0;
    }

    if (cmd == FUTEX_WAKE) {
        return (uint64_t)futex_wake_addr(addr, (uint32_t)val);
    }

    return (uint64_t)-(int64_t)38; /* ENOSYS for unsupported ops */
}
```

- [ ] **Step 3: Add `futex.c` to Makefile**

Add `kernel/syscall/futex.c` to `USERSPACE_SRCS` in Makefile.

- [ ] **Step 4: Add syscall dispatch**

In `syscall.c`, add:
```c
case 202: return sys_futex(arg1, arg2, arg3, arg4, arg5, arg6);
```

Add ARM64 translation:
```c
case  98: num = 202; break;  /* futex */
```

Add declaration in `sys_impl.h`:
```c
uint64_t sys_futex(uint64_t a1, uint64_t a2, uint64_t a3,
                   uint64_t a4, uint64_t a5, uint64_t a6);
```

- [ ] **Step 5: Build and run `make test`**

Expected: PASS. Futex is registered but not called yet by existing code.

- [ ] **Step 6: Commit**

```bash
git add kernel/syscall/futex.h kernel/syscall/futex.c \
        kernel/syscall/syscall.c kernel/syscall/sys_impl.h Makefile
git commit -m "feat: add futex WAIT/WAKE syscall (202)

Static pool of 64 waiter slots, 32-bucket hash by VA. FUTEX_PRIVATE_FLAG
stripped and ignored. futex_wake_addr exposed for clear_child_tid path."
```

---

## Task 5: Implement `sys_clone` with `CLONE_VM`

The main event. Refactor `sys_fork` into `sys_clone` that handles all thread creation flags.

**Files:**
- Modify: `kernel/syscall/sys_process.c`
- Modify: `kernel/syscall/syscall.c`
- Modify: `kernel/syscall/sys_impl.h`

- [ ] **Step 1: Add `sys_clone` function**

In `sys_process.c`, add `sys_clone` above `sys_fork`. The function handles both fork (no CLONE_VM) and thread creation (CLONE_VM). Key logic:

```c
/* Clone flag constants */
#define CLONE_VM             0x00000100u
#define CLONE_FS             0x00000200u
#define CLONE_FILES          0x00000400u
#define CLONE_SIGHAND        0x00000800u
#define CLONE_VFORK          0x00004000u
#define CLONE_SETTLS          0x00080000u
#define CLONE_PARENT_SETTID  0x00100000u
#define CLONE_CHILD_CLEARTID 0x00200000u
#define CLONE_THREAD         0x00010000u
#define CLONE_SYSVSEM        0x00040000u
#define CLONE_CHILD_SETTID   0x01000000u

uint64_t
sys_clone(syscall_frame_t *frame, uint64_t flags, uint64_t child_stack,
          uint64_t ptid, uint64_t ctid, uint64_t tls)
{
    aegis_task_t    *parent_task = sched_current();
    if (!parent_task || !parent_task->is_user) return (uint64_t)-(int64_t)1;
    aegis_process_t *parent = (aegis_process_t *)parent_task;

    /* If no CLONE_VM, delegate to existing fork logic. */
    uint32_t fl = (uint32_t)flags & ~0xFFu; /* strip signal number in low byte */
    if (!(fl & CLONE_VM))
        return sys_fork(frame);

    /* Capability gate */
    if (cap_check(parent->caps, CAP_TABLE_SIZE,
                  CAP_KIND_THREAD_CREATE, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    /* Process limit */
    if (s_fork_count >= MAX_PROCESSES)
        return (uint64_t)(int64_t)-11;

    /* Allocate child PCB (aegis_process_t) */
    aegis_process_t *child = kva_alloc_pages(1);
    if (!child) return (uint64_t)-(int64_t)12;

    /* Share PML4 (CLONE_VM) — no page table copy */
    child->pml4_phys = parent->pml4_phys;

    /* Share or copy fd table */
    if (fl & CLONE_FILES) {
        fd_table_ref(parent->fd_table);
        child->fd_table = parent->fd_table;
    } else {
        child->fd_table = fd_table_copy(parent->fd_table);
        if (!child->fd_table) { kva_free_pages(child, 1); return (uint64_t)-(int64_t)12; }
    }

    /* Copy caps (threads share the same cap table by value) */
    uint32_t ci;
    for (ci = 0; ci < CAP_TABLE_SIZE; ci++)
        child->caps[ci] = parent->caps[ci];
    for (ci = 0; ci < CAP_TABLE_SIZE; ci++) {
        child->exec_caps[ci].kind = CAP_KIND_NULL;
        child->exec_caps[ci].rights = 0;
    }

    /* Copy process fields */
    child->brk       = parent->brk;
    child->mmap_base = parent->mmap_base;
    __builtin_memcpy(child->cwd, parent->cwd, sizeof(parent->cwd));
    child->pid       = proc_alloc_pid();
    child->ppid      = parent->pid;
    child->uid       = parent->uid;
    child->gid       = parent->gid;
    child->pgid      = parent->pgid;
    child->umask     = parent->umask;
    child->stop_signum = 0;
    child->exit_status = 0;

    /* Thread group */
    if (fl & CLONE_THREAD) {
        child->tgid = parent->tgid;
        parent->thread_count++;
    } else {
        child->tgid = child->pid;
    }
    child->thread_count = 0; /* non-leader threads have count 0; leader tracks */

    /* Signal state */
    if (fl & CLONE_SIGHAND) {
        /* Share signal handlers — copy by value for now (shallow share).
         * A full share would need a refcounted sigaction table, which is
         * Phase 32 work. For musl pthreads this is sufficient because
         * threads rarely modify signal handlers independently. */
        __builtin_memcpy(child->sigactions, parent->sigactions, sizeof(parent->sigactions));
    } else {
        __builtin_memcpy(child->sigactions, parent->sigactions, sizeof(parent->sigactions));
    }
    child->signal_mask     = parent->signal_mask;
    child->pending_signals = 0;

    /* Task fields */
    child->task.state       = TASK_RUNNING;
    child->task.waiting_for = 0;
    child->task.is_user     = 1;
    child->task.tid         = child->pid;
    child->task.stack_pages = 4;

    /* TLS */
    if (fl & CLONE_SETTLS)
        child->task.fs_base = tls;
    else
        child->task.fs_base = parent_task->fs_base;

    /* clear_child_tid */
    child->task.clear_child_tid = (fl & CLONE_CHILD_CLEARTID) ? ctid : 0;

    /* Allocate kernel stack */
    uint8_t *kstack = kva_alloc_pages(4);
    if (!kstack) {
        if (!(fl & CLONE_FILES)) fd_table_unref(child->fd_table);
        kva_free_pages(child, 1);
        return (uint64_t)-(int64_t)12;
    }

    /* Build child kernel stack frame — same as sys_fork but with
     * child_stack as the user RSP if provided. */
    /* ... (reuse existing fork frame-building code, substituting
     *      child_stack for frame->user_rsp when child_stack != 0) ... */

    /* Register child with scheduler */
    child->task.stack_base      = kstack;
    child->task.kernel_stack_top = (uint64_t)(uintptr_t)(kstack + 4 * 4096);
    sched_add(&child->task);
    s_fork_count++;

    /* CLONE_PARENT_SETTID: write child TID to parent's user space */
    if ((fl & CLONE_PARENT_SETTID) && ptid) {
        uint32_t tid_val = child->pid;
        vmm_write_user_bytes(parent->pml4_phys, ptid, &tid_val, sizeof(tid_val));
    }

    /* CLONE_CHILD_SETTID: write child TID to child's user space
     * (same address space since CLONE_VM) */
    if ((fl & CLONE_CHILD_SETTID) && ctid) {
        uint32_t tid_val = child->pid;
        vmm_write_user_bytes(child->pml4_phys, ctid, &tid_val, sizeof(tid_val));
    }

    /* CLONE_VFORK: block parent until child exits or execs */
    if (fl & CLONE_VFORK) {
        parent_task->state = TASK_BLOCKED;
        /* Child will wake us on exit or exec — store in waiting_for */
        parent_task->waiting_for = child->pid;
        sched_block();
    }

    return (uint64_t)child->pid;
}
```

Note: The frame-building code (the long block that constructs the ISR frame on the child's kernel stack) should be extracted from sys_fork and reused. The only difference is that `child_stack` replaces `frame->user_rsp` in the iretq frame when `child_stack != 0`.

- [ ] **Step 2: Wire up `sys_clone` in syscall dispatch**

In `syscall.c`:
```c
case 56: return sys_clone(frame, arg1, arg2, arg3, arg4, arg5);
```

Update the ARM64 translation (line 61):
```c
case 220: num = 56; break;  /* clone */
```

Remove or redirect `case 57` (fork) — keep it calling `sys_fork` for backwards compat:
```c
case 57: return sys_fork(frame);
```

- [ ] **Step 3: Add declaration to `sys_impl.h`**

```c
uint64_t sys_clone(syscall_frame_t *frame, uint64_t flags, uint64_t child_stack,
                   uint64_t ptid, uint64_t ctid, uint64_t tls);
```

- [ ] **Step 4: Build and run `make test`**

Expected: PASS. Existing fork still works (clone without CLONE_VM delegates to fork). No existing code calls syscall 56 directly.

- [ ] **Step 5: Commit**

```bash
git add kernel/syscall/sys_process.c kernel/syscall/syscall.c kernel/syscall/sys_impl.h
git commit -m "feat: implement sys_clone with CLONE_VM for thread creation

Handles CLONE_VM, CLONE_FILES, CLONE_SIGHAND, CLONE_THREAD, CLONE_SETTLS,
CLONE_PARENT_SETTID, CLONE_CHILD_SETTID, CLONE_CHILD_CLEARTID, CLONE_VFORK.
Without CLONE_VM, delegates to sys_fork. CAP_KIND_THREAD_CREATE gated."
```

---

## Task 6: Thread exit + `exit_group` + `clear_child_tid`

Handle thread lifecycle: single thread exit decrements thread_count, exit_group kills all threads, clear_child_tid does futex wake for pthread_join.

**Files:**
- Modify: `kernel/syscall/sys_process.c` (sys_exit, sys_exit_group)
- Modify: `kernel/sched/sched.c` (sched_exit)

- [ ] **Step 1: Update `sys_exit` for thread awareness**

```c
uint64_t
sys_exit(uint64_t arg1)
{
    if (sched_current()->is_user) {
        aegis_process_t *proc = (aegis_process_t *)sched_current();
        proc->exit_status = arg1 & 0xFF;

        /* clear_child_tid: write 0 and futex wake (for pthread_join) */
        if (sched_current()->clear_child_tid) {
            uint32_t zero = 0;
            vmm_write_user_bytes(proc->pml4_phys,
                                 sched_current()->clear_child_tid,
                                 &zero, sizeof(zero));
            futex_wake_addr(sched_current()->clear_child_tid, 1);
        }

        if (proc->tgid == proc->pid) {
            /* Thread group leader exiting — PID 1 check */
            if (proc->pid == 1) {
                printk("[INIT] PID 1 exited with status %u — halting\n",
                       (uint32_t)(arg1 & 0xFF));
                arch_request_shutdown();
            }
        }
    }
    sched_exit();
    __builtin_unreachable();
}
```

- [ ] **Step 2: Update `sys_exit_group` to kill all threads**

```c
uint64_t
sys_exit_group(uint64_t arg1)
{
    aegis_task_t *cur = sched_current();
    if (cur->is_user) {
        aegis_process_t *proc = (aegis_process_t *)cur;
        proc->exit_status = arg1 & 0xFF;

        /* Kill all other threads in the same thread group */
        uint32_t my_tgid = proc->tgid;
        aegis_task_t *t = cur->next;
        while (t != cur) {
            if (t->is_user) {
                aegis_process_t *tp = (aegis_process_t *)t;
                if (tp->tgid == my_tgid && t != cur) {
                    t->state = TASK_ZOMBIE;
                }
            }
            t = t->next;
        }

        if (proc->pid == 1 || proc->tgid == 1) {
            printk("[INIT] PID 1 exited with status %u — halting\n",
                   (uint32_t)(arg1 & 0xFF));
            arch_request_shutdown();
        }
    }
    sched_exit();
    __builtin_unreachable();
}
```

- [ ] **Step 3: Update `sched_exit` for thread-aware cleanup**

In `sched.c`, the user-exit path (lines 141-235) needs to handle threads:
- If this is NOT the thread group leader AND other threads exist: don't free PML4 or send SIGCHLD; just free kernel stack.
- If this IS the last thread (or leader and thread_count drops to 0): do full cleanup.

Key change in the fd close section:
```c
    /* Use fd_table_unref instead of manual fd close loop */
    fd_table_unref(dying->fd_table);
    dying->fd_table = (fd_table_t *)0;
```

And the PML4 free section: only free PML4 if `dying->tgid == dying->pid` (leader) and no other threads share it.

- [ ] **Step 4: Build and run `make test`**

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add kernel/syscall/sys_process.c kernel/sched/sched.c
git commit -m "feat: thread-aware exit + exit_group + clear_child_tid

sys_exit handles clear_child_tid (futex wake for pthread_join).
sys_exit_group kills all threads in the group. sched_exit uses
fd_table_unref and thread-aware PML4 cleanup."
```

---

## Task 7: Test program + integration test

Build a pthread test binary, add to ext2 disk, write test_threads.py.

**Files:**
- Create: `user/thread_test/main.c`
- Create: `user/thread_test/Makefile`
- Create: `tests/test_threads.py`
- Modify: `Makefile` (disk target: add thread_test to ext2)

- [ ] **Step 1: Create `user/thread_test/main.c`**

```c
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>

static void *thread_fn(void *arg)
{
    int *val = (int *)arg;
    *val = 42;
    return (void *)0;
}

int main(void)
{
    /* Test 1: basic pthread create + join */
    int result = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, thread_fn, &result) != 0) {
        printf("THREAD FAIL: create\n");
        return 1;
    }
    if (pthread_join(t, NULL) != 0) {
        printf("THREAD FAIL: join\n");
        return 1;
    }
    if (result == 42)
        printf("THREAD OK\n");
    else
        printf("THREAD FAIL: result=%d\n", result);

    /* Test 2: DNS resolution (musl uses clone internally) */
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    int r = getaddrinfo("10.0.2.3", "53", &hints, &res);
    if (r == 0) {
        printf("DNS OK\n");
        freeaddrinfo(res);
    } else {
        printf("DNS FAIL: %d\n", r);
    }

    return 0;
}
```

- [ ] **Step 2: Create `user/thread_test/Makefile`**

```makefile
CC     = musl-gcc
CFLAGS = -static -O2 -fno-pie -no-pie -Wl,--build-id=none -lpthread

thread_test.elf: main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f thread_test.elf
```

- [ ] **Step 3: Add thread_test to ext2 disk in Makefile**

In the `$(DISK)` target's debugfs section, add:
```
write user/thread_test/thread_test.elf /bin/thread_test
```

- [ ] **Step 4: Create `tests/test_threads.py`**

Python test that boots with vigil, logs in, runs `/bin/thread_test`, checks for "THREAD OK" and "DNS OK" in output. Follow the pattern of existing tests (test_socket.py, test_pipe.py).

- [ ] **Step 5: Build on remote x86 machine, run test**

```bash
# On remote:
make -C user/thread_test
make disk
make INIT=vigil iso
python3 tests/test_threads.py
```

Expected: "THREAD OK" and "DNS OK" in output.

- [ ] **Step 6: Add test to `tests/run_tests.sh`**

```bash
echo "--- test_threads ---"
python3 tests/test_threads.py
```

- [ ] **Step 7: Commit**

```bash
git add user/thread_test/ tests/test_threads.py tests/run_tests.sh Makefile
git commit -m "test: Phase 29 pthread + DNS integration test

thread_test binary: pthread_create+join, getaddrinfo via musl's
internal clone. test_threads.py boots vigil, runs binary, checks
THREAD OK and DNS OK."
```

---

## Task 8: Update CLAUDE.md

Update Build Status table, add Phase 29 forward constraints, update roadmap.

**Files:**
- Modify: `CLAUDE.md`
- Modify: `.claude/CLAUDE.md`

- [ ] **Step 1: Add Phase 29 to Build Status**

```
| Threads (Phase 29) | ✅ | clone(CLONE_VM); futex WAIT/WAKE; fd_table_t refcount; per-thread TLS; test_threads.py PASS |
```

- [ ] **Step 2: Add Phase 29 forward constraints**

```markdown
### Phase 29 forward-looking constraints

**No pthread_cancel.** Requires `tgkill` for per-thread signal delivery. Phase 32.

**Shared sigactions by copy, not reference.** Threads copy the signal handler
table at clone time. A sigaction() call in one thread does NOT affect siblings.
Correct shared-reference semantics requires a refcounted sigaction table (Phase 32).

**fd_table refcount is not atomic.** Single-core only. SMP needs atomic ops.

**Futex pool static (64 slots).** Sufficient for typical use. No dynamic growth.

**`-THREAD_CREATE` vigil sandbox syntax not implemented.** Design intent documented.
Vigil service config parsing unchanged. Future hardening work.

**CLONE_VFORK wakeup on exec not implemented.** Parent blocks until child exits.
Should also wake on execve. Low priority — musl DNS clone exits, doesn't exec.
```

- [ ] **Step 3: Update Phase roadmap**

Mark Phase 29 as Done.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md .claude/CLAUDE.md
git commit -m "docs: Phase 29 complete — threads, futex, per-thread TLS"
```
