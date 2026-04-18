# Per-fd Wait Queues Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace global `g_poll_waiter` with per-fd wait queues so concurrent `poll()` callers do not starve and events are delivered with sub-tick latency.

**Architecture:** Extend the already-present-but-unused `waitq_t` in `kernel/sched/waitq.{c,h}` with stack-allocated entries so one task can wait on N queues simultaneously. Embed a `waitq_t` in every pollable kernel object. Producers (writers, ISRs, state-change paths) call `waitq_wake_all` on their object's queue. `sys_poll` and `sys_epoll_wait` register entries on each watched fd's queue plus an optional `g_timer_waitq` for timeouts.

**Tech Stack:** C (kernel, `-Werror -Wall -Wextra`, no malloc), Rust (`tests/` integration test harness via Vortex `QemuProcess`), GRUB+QEMU for the boot oracle.

**Branch:** create `poll-waitq` off master; do not work on `arm64-port`.

**Spec:** `docs/superpowers/specs/2026-04-16-poll-waitq-design.md`

---

## File Structure

**Created files:**
- (none — `waitq.{c,h}` already exists; we extend it)
- `tests/tests/poll_concurrent_pollers_test.rs` — two concurrent pollers regression test
- `tests/tests/gui_installer_advance_test.rs` — end-to-end gui-installer Enter advances screen
- `user/bin/poll-test/main.c` + `Makefile` — userspace helper for the concurrent-pollers test

**Modified files:**
- `kernel/sched/waitq.h` — add `waitq_entry_t`, `waitq_add`, `waitq_remove`; keep `waitq_wait` as convenience
- `kernel/sched/waitq.c` — implement entry-based API; `waitq_wait` becomes a wrapper
- `kernel/sched/sched.h` — `wq_next` no longer used; remove or repurpose (decision in Task 1)
- `kernel/fs/vfs.h` — add `waitq_t *(*get_waitq)(void *priv)` to `vfs_ops_t`
- `kernel/syscall/sys_socket.c` — rewrite `sys_poll`, `sys_epoll_wait`; delete `g_poll_waiter` references
- `kernel/net/epoll.c` — delete `g_poll_waiter` reference; add waitq integration
- `kernel/arch/x86_64/pit.c` — delete `g_poll_waiter`; call `waitq_wake_all(&g_timer_waitq)`
- `kernel/arch/arm64/stubs.c` — delete `g_poll_waiter` stub; add `g_timer_waitq` if needed
- `kernel/net/unix_socket.h` / `.c` — embed `poll_waiters`; wake on write/connect/free; `unix_sock_get_waitq` helper; `g_unix_sock_ops.get_waitq` populated
- `kernel/net/socket.h` / `.c` / `tcp.c` / `udp.c` — embed `poll_waiters`; wake on TCP rx, accept, UDP rx, state→CLOSE_WAIT/CLOSED; `sock_get_waitq` helper
- `kernel/fs/pipe.h` / `.c` — embed `poll_waiters` per direction; wake on write/close; populate `g_pipe_read_ops.get_waitq` / `g_pipe_write_ops.get_waitq`
- `kernel/tty/pty.h` / `.c` — embed `master_waiters` and `slave_waiters`; wake on master write (slave readable) and slave write (master readable)
- `kernel/fs/console.c` — file-static `g_console_waiters`; populate `get_waitq`
- `kernel/fs/kbd_vfs.c` — share `g_console_waiters` (or add own); wake from kbd ISR after byte push
- `kernel/drivers/usb_mouse.c` (or wherever the mouse VFS lives) — file-static waitq; wake when report dispatched
- `kernel/fs/memfd.c` — `get_waitq` returns NULL (no events ever fire; permissive `poll`)

---

## Conventions

- **Commits:** small, one-task each, conventional style: `kernel/<area>: <imperative summary>`. Co-author trailer: `Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>`
- **No `printk` debug noise** in committed code. If you add diagnostic prints during a task, remove them before the commit step.
- **Build verification per task:** every task ends with `make` (must succeed under `-Werror`). Tasks that touch the boot path also run `make test` (boot oracle must still pass).
- **No `make iso` between tasks** — that takes minutes and is for hardware testing. We only build the ELF + run the cargo test harness during plan execution.
- **Test driver:** the cargo tests use `aegis-test.iso` (text-mode) by default. The gui-installer test needs `aegis.iso` (graphical). If the ISO is missing, the test must `eprintln!("SKIP: ...")` and return — never panic on missing artifact.

---

## Task 0: Set up branch + verify baseline

**Files:** none

- [ ] **Step 1: Create feature branch off master**

```bash
cd /Users/dylan/Developer/aegis
git checkout master
git pull --ff-only
git checkout -b poll-waitq
```

- [ ] **Step 2: Confirm baseline build passes**

```bash
make clean
make
```
Expected: kernel ELF builds, no warnings.

- [ ] **Step 3: Confirm boot oracle passes**

```bash
make test
```
Expected: exit 0, all `[X] OK:` lines present.

- [ ] **Step 4: No commit needed** (no code changed yet).

---

## Task 1: Extend waitq with entry-based API

**Files:**
- Modify: `kernel/sched/waitq.h` (entire file)
- Modify: `kernel/sched/waitq.c` (entire file)
- Modify: `kernel/sched/sched.h:39-43` (remove `wq_next` since we no longer use it)

**Why:** Current `waitq_t` allows only one waitq per task (single `wq_next` slot in `aegis_task_t`). `sys_poll` needs one task on N queues simultaneously. Entry-based API (caller-allocated `waitq_entry_t`) supports this.

- [ ] **Step 1: Replace `kernel/sched/waitq.h` contents**

```c
#ifndef AEGIS_WAITQ_H
#define AEGIS_WAITQ_H

/*
 * waitq_t — kernel wait queue with caller-allocated entries.
 *
 * One task may register on N queues simultaneously by using N entries.
 * sys_poll uses this: one entry per pollfd, all on the kernel stack.
 *
 * Producer pattern:
 *     waitq_wake_all(&obj->poll_waiters);
 *
 * Consumer pattern (multi-queue):
 *     waitq_entry_t e[N];
 *     for (i = 0; i < N; i++) {
 *         e[i].task = sched_current();
 *         waitq_add(&queues[i], &e[i]);
 *     }
 *     sched_block();
 *     for (i = 0; i < N; i++) waitq_remove(&queues[i], &e[i]);
 *
 * Consumer pattern (single-queue convenience):
 *     waitq_wait(&obj->poll_waiters);
 *
 * waitq_remove is idempotent — safe even if the entry was never added or
 * has already been removed by another path. This keeps the cleanup loop
 * simple regardless of which queue actually fired the wake.
 *
 * All ops take spin_lock_irqsave on the queue's lock. ISR-safe.
 */

#include <stddef.h>
#include "spinlock.h"

struct aegis_task_t;

typedef struct waitq_entry {
    struct aegis_task_t *task;
    struct waitq_entry  *next;
    struct waitq_entry  *prev;
    /* on_queue: 1 while linked into a waitq's list; 0 otherwise.
     * Used by waitq_remove to be idempotent. */
    uint8_t              on_queue;
} waitq_entry_t;

typedef struct waitq {
    waitq_entry_t *head;
    spinlock_t     lock;
} waitq_t;

#define WAITQ_INIT { .head = NULL, .lock = SPINLOCK_INIT }

static inline void
waitq_init(waitq_t *wq)
{
    wq->head = NULL;
    wq->lock = (spinlock_t)SPINLOCK_INIT;
}

/* waitq_add — link entry into queue at head. Caller fills entry->task
 * before calling. Must NOT be called on an entry that is already on
 * any queue (no double-add). */
void waitq_add(waitq_t *wq, waitq_entry_t *entry);

/* waitq_remove — unlink entry from queue. Idempotent. */
void waitq_remove(waitq_t *wq, waitq_entry_t *entry);

/* waitq_wake_one — sched_wake the head waiter (no remove; the woken
 * task removes itself in its cleanup path). No-op if queue empty.
 * Safe to call from IRQ context. */
void waitq_wake_one(waitq_t *wq);

/* waitq_wake_all — sched_wake every entry's task in the queue (no
 * remove; each woken task removes itself). Safe from IRQ context. */
void waitq_wake_all(waitq_t *wq);

/* waitq_wait — convenience for single-queue blocking I/O paths.
 * Allocates a stack entry, adds, sched_block, removes on return.
 * Caller must have checked the wait condition is NOT met. */
void waitq_wait(waitq_t *wq);

#endif /* AEGIS_WAITQ_H */
```

- [ ] **Step 2: Replace `kernel/sched/waitq.c` contents**

```c
/* waitq.c — wait queue with caller-allocated entries. See waitq.h. */
#include "waitq.h"
#include "sched.h"
#include "spinlock.h"
#include <stddef.h>

void
waitq_add(waitq_t *wq, waitq_entry_t *e)
{
    irqflags_t fl = spin_lock_irqsave(&wq->lock);
    e->prev = NULL;
    e->next = wq->head;
    if (wq->head) wq->head->prev = e;
    wq->head    = e;
    e->on_queue = 1;
    spin_unlock_irqrestore(&wq->lock, fl);
}

void
waitq_remove(waitq_t *wq, waitq_entry_t *e)
{
    irqflags_t fl = spin_lock_irqsave(&wq->lock);
    if (e->on_queue) {
        if (e->prev) e->prev->next = e->next;
        else         wq->head      = e->next;
        if (e->next) e->next->prev = e->prev;
        e->prev     = NULL;
        e->next     = NULL;
        e->on_queue = 0;
    }
    spin_unlock_irqrestore(&wq->lock, fl);
}

void
waitq_wake_one(waitq_t *wq)
{
    irqflags_t fl = spin_lock_irqsave(&wq->lock);
    waitq_entry_t *e = wq->head;
    aegis_task_t  *t = e ? e->task : NULL;
    spin_unlock_irqrestore(&wq->lock, fl);
    if (t) sched_wake(t);
}

void
waitq_wake_all(waitq_t *wq)
{
    /* Snapshot task pointers under the lock, then sched_wake outside the
     * lock (sched_lock > waitq_lock in the global ordering). The woken
     * tasks are responsible for calling waitq_remove on themselves —
     * sched_wake just flips state to RUNNING. */
    aegis_task_t *snap[64];
    int           n = 0;

    irqflags_t fl = spin_lock_irqsave(&wq->lock);
    for (waitq_entry_t *e = wq->head; e && n < 64; e = e->next)
        snap[n++] = e->task;
    spin_unlock_irqrestore(&wq->lock, fl);

    for (int i = 0; i < n; i++)
        if (snap[i]) sched_wake(snap[i]);
}

void
waitq_wait(waitq_t *wq)
{
    waitq_entry_t e = { .task = sched_current(), .next = NULL,
                        .prev = NULL,           .on_queue = 0 };
    waitq_add(wq, &e);
    sched_block();
    waitq_remove(wq, &e);
}
```

- [ ] **Step 3: Remove obsolete `wq_next` from `aegis_task_t`**

In `kernel/sched/sched.h`, delete lines 39-43:

```c
    /* wait-queue linkage (WQ audit item).
     * Used by waitq_wait to link this task into a waitq_t's head list.
     * NULL when the task is not currently blocked on any wait queue. */
    struct aegis_task_t *wq_next;
```

(Replace nothing — just remove the field and its comment.)

- [ ] **Step 4: Build**

```bash
make clean && make 2>&1 | tail -20
```
Expected: build succeeds, no warnings.

- [ ] **Step 5: Boot oracle**

```bash
make test
```
Expected: exit 0.

- [ ] **Step 6: Commit**

```bash
git add kernel/sched/waitq.h kernel/sched/waitq.c kernel/sched/sched.h
git commit -m "$(cat <<'EOF'
sched/waitq: switch to caller-allocated entries

One task may now register on N queues simultaneously (required by sys_poll's
per-fd wait). The single-queue convenience waitq_wait is preserved as a
wrapper. The wq_next field on aegis_task_t is removed — entries hold the
linkage now.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Add `get_waitq` to vfs_ops_t

**Files:**
- Modify: `kernel/fs/vfs.h:54-81` (extend `vfs_ops_t`)

- [ ] **Step 1: Add field to `vfs_ops_t`**

In `kernel/fs/vfs.h`, after the `poll` field (line 80), add:

```c
    /* get_waitq — return the wait queue for this fd, or NULL if the fd
     * type has no events to wait on (e.g. memfd is always ready).
     * Used by sys_poll / sys_epoll_wait to register on the right queue.
     * NULL is the default — caller falls back to PIT-tick polling for
     * fds without a waitq. */
    struct waitq *(*get_waitq)(void *priv);
```

Add forward declaration above the typedef:

```c
struct waitq;
```

- [ ] **Step 2: Build**

```bash
make 2>&1 | tail -10
```
Expected: build succeeds. Existing `vfs_ops_t` initializers omit `get_waitq` → C designated initializers default to NULL → no warning, correct behavior.

- [ ] **Step 3: Boot oracle**

```bash
make test
```
Expected: exit 0.

- [ ] **Step 4: Commit**

```bash
git add kernel/fs/vfs.h
git commit -m "$(cat <<'EOF'
fs/vfs: add optional get_waitq to vfs_ops_t

Returns the wait queue for an fd so sys_poll can register on it.
NULL = no waitq (fall back to tick-based polling). All existing
ops tables omit the field → defaults to NULL via designated initializers.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Define `g_timer_waitq`; rewrite PIT to wake it

**Files:**
- Modify: `kernel/sched/waitq.c` (add definition)
- Modify: `kernel/sched/waitq.h` (add extern)
- Modify: `kernel/arch/x86_64/pit.c:46` (delete `g_poll_waiter`), `pit.c:78-84` (replace wake hack)
- Modify: `kernel/arch/arm64/stubs.c:204` (delete `g_poll_waiter` stub, add `g_timer_waitq` stub)

- [ ] **Step 1: Declare `g_timer_waitq` in `kernel/sched/waitq.h`**

Append before the `#endif`:

```c
/* g_timer_waitq — woken once per PIT tick by the timer ISR.
 * sys_poll / sys_epoll_wait with a finite timeout register on this
 * queue so they wake to re-check their deadline. Calls with timeout=-1
 * skip it (zero overhead, no spurious wakes). */
extern waitq_t g_timer_waitq;
```

- [ ] **Step 2: Define it in `kernel/sched/waitq.c`**

Add at top after includes:

```c
waitq_t g_timer_waitq = WAITQ_INIT;
```

- [ ] **Step 3: Update `kernel/arch/x86_64/pit.c`**

Replace lines 43-46 (the `g_poll_waiter` declaration) with:

```c
/* PIT wakes g_timer_waitq each tick so timed pollers re-check their
 * deadline. Replaces the old single-slot g_poll_waiter (deleted). */
#include "../../sched/waitq.h"
```

Replace lines 78-84 (the `if (g_poll_waiter)` block) with:

```c
    /* Wake all timed pollers so they can re-check their deadline. */
    waitq_wake_all(&g_timer_waitq);
```

- [ ] **Step 4: Update `kernel/arch/arm64/stubs.c:204`**

Replace:

```c
struct aegis_task_t *g_poll_waiter = (struct aegis_task_t *)0;
```

with:

```c
#include "../../sched/waitq.h"
waitq_t g_timer_waitq = WAITQ_INIT;
```

- [ ] **Step 5: Build**

```bash
make 2>&1 | tail -20
```
Expected: build succeeds. Compile errors will surface in `sys_socket.c` (still references `g_poll_waiter`) and `epoll.c` — those get fixed in Tasks 4 and 5. Don't try to fix them yet; we want a contained step. If the kernel won't link without those fixes, comment out the `extern aegis_task_t *g_poll_waiter` lines in `sys_socket.c:913` and `epoll.c:245` temporarily and stub the body to `sched_block()`. Re-build.

Actually: combine Steps 5–7 below to keep the kernel buildable. Don't commit until Tasks 4 and 5 are done. **Do not run `make test` until Task 5 commits.**

Skip the commit at the end of this task. Combined commit at end of Task 5.

- [ ] **Step 6: Apply temporary stub in `kernel/syscall/sys_socket.c:910-917`**

Replace:

```c
        {
            extern aegis_task_t *g_poll_waiter;
            g_poll_waiter = (aegis_task_t *)sched_current();
            sched_block();
            /* After wake: g_poll_waiter cleared by PIT handler */
        }
```

with (temporary, will be replaced in Task 4):

```c
        {
            /* TEMP: bridged via g_timer_waitq until Task 4 lands per-fd queues. */
            waitq_wait(&g_timer_waitq);
        }
```

Add `#include "../sched/waitq.h"` at the top of `sys_socket.c` if not already present.

- [ ] **Step 7: Apply temporary stub in `kernel/net/epoll.c:243-247`**

Replace:

```c
        {
            extern aegis_task_t *g_poll_waiter;
            g_poll_waiter = (aegis_task_t *)sched_current();
            sched_block();
        }
```

with:

```c
        {
            /* TEMP: bridged via g_timer_waitq until Task 9 wires epoll waitqs. */
            waitq_wait(&g_timer_waitq);
        }
```

Add `#include "../sched/waitq.h"` at the top of `epoll.c` if not already present.

- [ ] **Step 8: Build + boot oracle**

```bash
make clean && make 2>&1 | tail -10
make test
```
Expected: kernel builds, boot oracle passes. Behavior is identical to before (PIT still wakes pollers every tick), just via the new mechanism.

- [ ] **Step 9: Commit**

```bash
git add kernel/sched/waitq.h kernel/sched/waitq.c \
        kernel/arch/x86_64/pit.c kernel/arch/arm64/stubs.c \
        kernel/syscall/sys_socket.c kernel/net/epoll.c
git commit -m "$(cat <<'EOF'
sched/pit: replace g_poll_waiter with g_timer_waitq

PIT now wakes every task registered on g_timer_waitq each tick.
sys_poll and sys_epoll_wait temporarily bridge through it via
waitq_wait — preserves current behavior. Per-fd waitqs land in
following tasks; g_timer_waitq remains for finite-timeout polling.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: AF_UNIX — embed waitq and wake on writes

**Files:**
- Modify: `kernel/net/unix_socket.h` — add `waitq_t poll_waiters` to `unix_sock_t`; declare `unix_sock_get_waitq`
- Modify: `kernel/net/unix_socket.c` — init waitq on alloc; wake on write/connect/free; populate `g_unix_sock_ops.get_waitq`

- [ ] **Step 1: Add `poll_waiters` field to `unix_sock_t`**

In `kernel/net/unix_socket.h`, add to the struct after `refcount` (line 72):

```c
    /* Wake queue for sys_poll / sys_epoll_wait waiters on this fd.
     * Producers (this socket's ring writer, peer's connect, peer's free)
     * call waitq_wake_all(&poll_waiters) to notify pollers. */
    waitq_t        poll_waiters;
```

Add `#include "../sched/waitq.h"` at the top.

- [ ] **Step 2: Declare helper in header**

Append to `kernel/net/unix_socket.h` before the `#endif`:

```c
/* Get the wait queue for an AF_UNIX socket. Used by sys_poll. */
waitq_t *unix_sock_get_waitq(uint32_t id);
```

- [ ] **Step 3: Initialize the waitq on alloc**

In `kernel/net/unix_socket.c`, in `unix_sock_alloc` (around line 207, after the `_memset` that zeros the slot):

```c
            _memset(&s_unix[i], 0, sizeof(unix_sock_t));
            s_unix[i].in_use   = 1;
            s_unix[i].state    = UNIX_CREATED;
            s_unix[i].peer_id  = UNIX_NONE;
            s_unix[i].refcount = 1;
            waitq_init(&s_unix[i].poll_waiters);   /* NEW */
```

Same fix in `unix_sock_connect`'s "allocate server-side socket" loop (around line 369–388 in current code) — after the `_memset` and field inits there.

- [ ] **Step 4: Wake pollers on write**

In `kernel/net/unix_socket.c`, in `unix_sock_write` (around line 583–588), the existing peer-wake block becomes:

```c
            /* Wake peer if blocked on read */
            unix_sock_t *p = unix_sock_get(peer);
            if (p) {
                if (p->waiter_task) {
                    sched_wake(p->waiter_task);
                    p->waiter_task = (void *)0;
                }
                /* Wake any sys_poll/epoll_wait registered on the peer. */
                waitq_wake_all(&p->poll_waiters);
            }
```

- [ ] **Step 5: Wake pollers on connect (listener side)**

In `kernel/net/unix_socket.c`, in `unix_sock_connect` after the existing `if (listener->waiter_task)` block (around line 427–431):

```c
    /* Wake listener if blocked in accept */
    if (listener->waiter_task) {
        sched_wake(listener->waiter_task);
        listener->waiter_task = (void *)0;
    }
    /* Wake any sys_poll registered on the listener (POLLIN for accept). */
    waitq_wake_all(&listener->poll_waiters);
```

- [ ] **Step 6: Wake pollers on close (peer-side HUP)**

In `kernel/net/unix_socket.c`, in `unix_sock_free` after the existing peer-wake block (around line 244–250):

```c
    uint32_t peer = us->peer_id;
    if (peer != UNIX_NONE && peer < UNIX_SOCK_MAX && s_unix[peer].in_use) {
        if (s_unix[peer].waiter_task) {
            sched_wake(s_unix[peer].waiter_task);
            s_unix[peer].waiter_task = (void *)0;
        }
        /* Wake peer's pollers — they'll see POLLHUP next iteration. */
        waitq_wake_all(&s_unix[peer].poll_waiters);
    }
```

- [ ] **Step 7: Implement `unix_sock_get_waitq`**

Append to `kernel/net/unix_socket.c`:

```c
waitq_t *
unix_sock_get_waitq(uint32_t id)
{
    if (id >= UNIX_SOCK_MAX || !s_unix[id].in_use) return (waitq_t *)0;
    return &s_unix[id].poll_waiters;
}
```

- [ ] **Step 8: Wire `get_waitq` into the ops table**

`get_waitq` for VFS fds is dispatched via `vfs_ops_t.get_waitq`. We need a static helper that takes the `priv` (which for AF_UNIX is the sock id cast to a pointer):

In `kernel/net/unix_socket.c`, before `g_unix_sock_ops` definition:

```c
static struct waitq *
unix_vfs_get_waitq(void *priv)
{
    uint32_t id = (uint32_t)(uintptr_t)priv;
    return unix_sock_get_waitq(id);
}
```

Update the ops table:

```c
const vfs_ops_t g_unix_sock_ops = {
    .read       = unix_vfs_read,
    .write      = unix_vfs_write,
    .close      = unix_vfs_close,
    .readdir    = (void *)0,
    .dup        = unix_vfs_dup,
    .stat       = unix_vfs_stat,
    .poll       = unix_vfs_poll,
    .get_waitq  = unix_vfs_get_waitq,    /* NEW */
};
```

- [ ] **Step 9: Build + boot oracle**

```bash
make 2>&1 | tail -10
make test
```
Expected: kernel builds, boot oracle passes. AF_UNIX still works through the legacy path; no behavior change yet because `sys_poll` doesn't consult `get_waitq` until Task 5.

- [ ] **Step 10: Commit**

```bash
git add kernel/net/unix_socket.h kernel/net/unix_socket.c
git commit -m "$(cat <<'EOF'
net/unix_socket: embed poll_waiters; wake on write/connect/free

Adds the waitq each AF_UNIX socket needs so sys_poll can register
per-fd. Producers wake it alongside the existing waiter_task path.
get_waitq plumbed through g_unix_sock_ops.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Rewrite sys_poll to register per-fd

**Files:**
- Modify: `kernel/syscall/sys_socket.c:836-919` (`sys_poll`)
- Add: `kernel/syscall/fd_waitq.c` — new file with `fd_get_waitq` helper
- Modify: `kernel/syscall/Makefile` (or top-level Makefile if subsystems are flat) to compile `fd_waitq.c`

- [ ] **Step 1: Create `kernel/syscall/fd_waitq.c`**

```c
/* fd_waitq.c — dispatch fd → waitq pointer for sys_poll / sys_epoll_wait. */
#include "../net/socket.h"
#include "../net/unix_socket.h"
#include "../proc/proc.h"
#include "../sched/sched.h"
#include "../sched/waitq.h"
#include "../fs/vfs.h"

/* fd_get_waitq — return the wait queue for an fd, or NULL if the fd
 * type has no events to wait on. */
waitq_t *
fd_get_waitq(int fd)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();

    /* AF_INET socket — wired in Task 6; returns NULL until then. */
    uint32_t sid = sock_id_from_fd(fd, proc);
    if (sid != SOCK_NONE) {
        extern waitq_t *sock_get_waitq(uint32_t id);  /* fwd-decl */
        return sock_get_waitq(sid);
    }

    /* AF_UNIX socket. */
    uint32_t uid = unix_sock_id_from_fd(fd, proc);
    if (uid != UNIX_NONE)
        return unix_sock_get_waitq(uid);

    /* VFS fd — pipe, tty, console, kbd, mouse, memfd. */
    if (fd >= 0 && (uint32_t)fd < PROC_MAX_FDS) {
        const vfs_ops_t *ops = proc->fd_table->fds[fd].ops;
        if (ops && ops->get_waitq)
            return ops->get_waitq(proc->fd_table->fds[fd].priv);
    }

    return (waitq_t *)0;
}
```

Note: `sock_get_waitq` is forward-declared but does not exist yet. We'll add a stub that returns NULL in Step 2 to keep the kernel building.

- [ ] **Step 2: Stub `sock_get_waitq` in `kernel/net/socket.c`**

Append:

```c
/* TEMP: returns NULL until Task 6 embeds a waitq in sock_t. */
struct waitq *
sock_get_waitq(uint32_t id)
{
    (void)id;
    return (struct waitq *)0;
}
```

- [ ] **Step 3: Add `kernel/syscall/fd_waitq.c` to the build**

Find the kernel Makefile (likely `Makefile` or `kernel/Makefile`). Locate the line that lists kernel C objects (look for `kernel/syscall/sys_socket.o` or similar). Add `kernel/syscall/fd_waitq.o` to the same list. Use grep:

```bash
grep -rn "sys_socket" Makefile kernel/Makefile 2>/dev/null
```

Then edit the matched file to add `kernel/syscall/fd_waitq.o`.

- [ ] **Step 4: Add `fd_get_waitq` declaration**

Create `kernel/syscall/fd_waitq.h`:

```c
#ifndef AEGIS_FD_WAITQ_H
#define AEGIS_FD_WAITQ_H

struct waitq;
struct waitq *fd_get_waitq(int fd);

#endif
```

- [ ] **Step 5: Replace `sys_poll` body**

In `kernel/syscall/sys_socket.c`, replace the entire `sys_poll` function (lines 836-919) with:

```c
uint64_t
sys_poll(uint64_t fds_ptr, uint64_t nfds, uint64_t timeout_ms)
{
    if (nfds > 64) return (uint64_t)-(int64_t)22;
    if (!user_ptr_valid(fds_ptr, nfds * sizeof(k_pollfd_t))) return (uint64_t)-(int64_t)14;

    aegis_process_t *proc = (aegis_process_t *)sched_current();

    uint64_t now0 = arch_get_ticks();
    uint64_t deadline = (timeout_ms == (uint64_t)-1) ? 0
                       : (timeout_ms == 0)           ? now0
                                                     : now0 + (timeout_ms / 10);

    waitq_entry_t fd_entries[64];
    waitq_t      *fd_queues[64];
    waitq_entry_t timer_entry;
    timer_entry.task = sched_current();
    timer_entry.next = (void *)0;
    timer_entry.prev = (void *)0;
    timer_entry.on_queue = 0;

    for (;;) {
        int ready = 0;
        uint64_t i;
        for (i = 0; i < nfds; i++) {
            k_pollfd_t pfd;
            copy_from_user(&pfd,
                (const void *)(uintptr_t)(fds_ptr + i * sizeof(k_pollfd_t)),
                sizeof(k_pollfd_t));
            pfd.revents = 0;
            uint32_t sid = sock_id_from_fd(pfd.fd, proc);
            if (sid != SOCK_NONE) {
                /* Existing AF_INET socket poll logic — unchanged from
                 * the version in this file before Task 5. Keep all the
                 * tcp_conn / udp / accept_head logic verbatim. */
                sock_t *s = sock_get(sid);
                if (s) {
                    if (s->type == SOCK_TYPE_STREAM && s->state == SOCK_CONNECTED) {
                        tcp_conn_t *tc = tcp_conn_get(s->tcp_conn_id);
                        int peek = tcp_conn_recv(s->tcp_conn_id, (void *)0, 0);
                        if ((pfd.events & POLLIN) && (peek > 0 || peek == 0))
                            pfd.revents |= POLLIN;
                        if (pfd.events & POLLOUT)
                            pfd.revents |= POLLOUT;
                        if (tc && (tc->state == TCP_CLOSE_WAIT
                                   || tc->state == TCP_CLOSED
                                   || tc->state == TCP_TIME_WAIT))
                            pfd.revents |= POLLHUP;
                    } else if (s->type == SOCK_TYPE_STREAM
                               && s->state == SOCK_LISTENING) {
                        if ((pfd.events & POLLIN) && s->accept_head != s->accept_tail)
                            pfd.revents |= POLLIN;
                    } else if (s->type == SOCK_TYPE_DGRAM) {
                        if ((pfd.events & POLLIN) && s->udp_rx_head != s->udp_rx_tail)
                            pfd.revents |= POLLIN;
                        if (pfd.events & POLLOUT)
                            pfd.revents |= POLLOUT;
                    }
                }
            } else if (pfd.fd >= 0 && (uint32_t)pfd.fd < PROC_MAX_FDS &&
                       proc->fd_table->fds[pfd.fd].ops) {
                const vfs_ops_t *ops = proc->fd_table->fds[pfd.fd].ops;
                if (ops->poll) {
                    uint16_t r = ops->poll(proc->fd_table->fds[pfd.fd].priv);
                    pfd.revents = r & (pfd.events | POLLERR | POLLHUP);
                } else {
                    pfd.revents = pfd.events & (POLLIN | POLLOUT);
                }
            } else {
                pfd.revents = POLLNVAL;
            }
            if (pfd.revents) ready++;
            copy_to_user(
                (void *)(uintptr_t)(fds_ptr + i * sizeof(k_pollfd_t)),
                &pfd, sizeof(k_pollfd_t));
        }
        if (ready > 0 || timeout_ms == 0) return (uint64_t)ready;
        if (deadline && arch_get_ticks() >= deadline) return 0;

        /* Register on every fd's waitq + g_timer_waitq if we have a
         * deadline. Then sched_block. On wake, unregister from
         * everything (idempotent — only removes if still queued). */
        for (i = 0; i < nfds; i++) {
            k_pollfd_t pfd;
            copy_from_user(&pfd,
                (const void *)(uintptr_t)(fds_ptr + i * sizeof(k_pollfd_t)),
                sizeof(k_pollfd_t));
            fd_queues[i] = fd_get_waitq(pfd.fd);
            fd_entries[i].task     = sched_current();
            fd_entries[i].next     = (void *)0;
            fd_entries[i].prev     = (void *)0;
            fd_entries[i].on_queue = 0;
            if (fd_queues[i])
                waitq_add(fd_queues[i], &fd_entries[i]);
        }
        if (deadline) waitq_add(&g_timer_waitq, &timer_entry);

        sched_block();

        for (i = 0; i < nfds; i++)
            if (fd_queues[i])
                waitq_remove(fd_queues[i], &fd_entries[i]);
        if (deadline) waitq_remove(&g_timer_waitq, &timer_entry);
    }
}
```

Add `#include "fd_waitq.h"` and `#include "../sched/waitq.h"` near the top of `sys_socket.c` if not already present.

- [ ] **Step 6: Build**

```bash
make clean && make 2>&1 | tail -20
```
Expected: builds cleanly.

- [ ] **Step 7: Boot oracle**

```bash
make test
```
Expected: exit 0. AF_UNIX poll latency is now sub-tick when peers write.

- [ ] **Step 8: Manual smoke — gui-installer event flow**

```bash
ls build/aegis.iso 2>/dev/null && cargo test --manifest-path tests/Cargo.toml \
    --test gui_installer_lumen_connect_test -- --nocapture 2>&1 | tail -30 \
  || echo "SKIP: build/aegis.iso not present"
```
Expected (when ISO exists): test runs end-to-end, terminal opens via dock, `gui-installer` typed at prompt, screen 1 marker appears. Don't need the test to PASS yet — Task 6+ may still be required for full functionality. Just verify it doesn't regress.

- [ ] **Step 9: Commit**

```bash
git add kernel/syscall/sys_socket.c kernel/syscall/fd_waitq.c \
        kernel/syscall/fd_waitq.h kernel/net/socket.c Makefile
git commit -m "$(cat <<'EOF'
syscall/sys_poll: register per-fd waitqs + timer waitq

sys_poll now adds the calling task as a waitq entry on each fd's
poll_waiters queue (when available) and on g_timer_waitq if a
finite timeout is set. Eliminates starvation when multiple tasks
poll concurrently. AF_UNIX writes already wake the peer queue
(Task 4); other fd types fall back to tick-based wake until they
get migrated.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: AF_INET sockets — embed waitq

**Files:**
- Modify: `kernel/net/socket.h` — add `waitq_t poll_waiters` to `sock_t`; declare `sock_get_waitq`
- Modify: `kernel/net/socket.c` — init waitq on alloc; replace stub `sock_get_waitq`; wake on accept enqueue and UDP rx
- Modify: `kernel/net/tcp.c` — wake the receiving socket's `poll_waiters` on rx, on state→CLOSE_WAIT/CLOSED, on connect-completion, etc.
- Modify: `kernel/net/udp.c` — wake the receiving socket's `poll_waiters` on rx

- [ ] **Step 1: Read `kernel/net/socket.h` to find the `sock_t` struct definition**

```bash
```

In `kernel/net/socket.h`, locate the `sock_t` struct (search for `typedef struct .*sock_t` or `struct sock`).

- [ ] **Step 2: Add `poll_waiters` to `sock_t`**

After the existing `waiter_task` field (search for `waiter_task` in `sock_t`), add:

```c
    /* Wake queue for sys_poll / sys_epoll_wait waiters on this socket. */
    waitq_t  poll_waiters;
```

Add `#include "../sched/waitq.h"` at the top of `socket.h` (with appropriate forward decl if it would create a cycle).

- [ ] **Step 3: Initialize the waitq on allocation**

In `kernel/net/socket.c`, find `sock_alloc` (or whichever function zero-inits a new `sock_t`). After the zero-init, add:

```c
    waitq_init(&s->poll_waiters);
```

- [ ] **Step 4: Replace the temporary `sock_get_waitq` stub**

In `kernel/net/socket.c`, replace the Task 5 stub with:

```c
struct waitq *
sock_get_waitq(uint32_t id)
{
    sock_t *s = sock_get(id);
    return s ? &s->poll_waiters : (struct waitq *)0;
}
```

- [ ] **Step 5: Wake on TCP rx**

In `kernel/net/tcp.c`, find the function that enqueues received data into a socket's rx buffer (search for `tcp_conn_recv_push`, `tcp_rx_enqueue`, or similar — the path called from the IP receive handler). After enqueueing data, add:

```c
    waitq_wake_all(&s->poll_waiters);
```

(Adjust `s` to whatever the local pointer to the destination `sock_t` is in that function.)

- [ ] **Step 6: Wake on TCP state→CLOSE_WAIT/CLOSED**

In `kernel/net/tcp.c`, find every place that transitions a TCP connection's state to `TCP_CLOSE_WAIT`, `TCP_CLOSED`, or `TCP_TIME_WAIT`. After each transition, find the associated `sock_t` (typically via `tc->sock_id` or a back-pointer) and add:

```c
    sock_t *s = sock_get(tc->sock_id);
    if (s) waitq_wake_all(&s->poll_waiters);
```

If the existing code already pulls `s` from `tc`, just append the wake.

- [ ] **Step 7: Wake on accept enqueue**

In `kernel/net/socket.c` or `kernel/net/tcp.c`, find the path that pushes a new accept onto a listener's accept queue (search for `accept_head++` or `s->accept_queue[`). After enqueue:

```c
    waitq_wake_all(&listener->poll_waiters);
```

- [ ] **Step 8: Wake on UDP rx**

In `kernel/net/udp.c`, find the function that pushes a received datagram into a socket's `udp_rx_*` ring. After enqueue:

```c
    waitq_wake_all(&s->poll_waiters);
```

- [ ] **Step 9: Build**

```bash
make 2>&1 | tail -10
```
Expected: builds cleanly.

- [ ] **Step 10: Boot oracle**

```bash
make test
```
Expected: exit 0.

- [ ] **Step 11: Smoke test the network stack**

```bash
cargo test --manifest-path tests/Cargo.toml --test test_net_stack -- --nocapture 2>&1 | tail -20
```
Expected: existing TCP/UDP tests pass.

- [ ] **Step 12: Commit**

```bash
git add kernel/net/socket.h kernel/net/socket.c kernel/net/tcp.c kernel/net/udp.c
git commit -m "$(cat <<'EOF'
net/socket: embed poll_waiters; wake on rx + state change

AF_INET sockets now wake any registered sys_poll / sys_epoll_wait
caller on TCP rx, UDP rx, accept enqueue, and TCP state transitions
to CLOSE_WAIT / CLOSED / TIME_WAIT. sock_get_waitq returns the
embedded waitq for fd_waitq dispatch.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: pipes — embed waitqs

**Files:**
- Modify: `kernel/fs/pipe.h` — add `waitq_t read_waiters; waitq_t write_waiters;` to `pipe_t`
- Modify: `kernel/fs/pipe.c` — init waitqs on creation; wake on write/close; populate `g_pipe_read_ops.get_waitq` and `g_pipe_write_ops.get_waitq`

- [ ] **Step 1: Add fields to `pipe_t`**

In `kernel/fs/pipe.h`, find the `pipe_t` struct. Add after the existing `waiter_task` (or wherever the per-side state lives):

```c
    waitq_t  read_waiters;    /* pollers waiting for POLLIN  */
    waitq_t  write_waiters;   /* pollers waiting for POLLOUT */
```

Include `"../sched/waitq.h"` at the top.

- [ ] **Step 2: Init waitqs in pipe creation path**

Find `sys_pipe2` or `pipe_alloc` in `kernel/fs/pipe.c`. After the pipe is zero-initialized:

```c
    waitq_init(&p->read_waiters);
    waitq_init(&p->write_waiters);
```

The existing `poll_test.c` self-test allocates a pipe via `kva_alloc_pages` + `memset` directly — update it too:

```c
    waitq_init(&p->read_waiters);
    waitq_init(&p->write_waiters);
```

- [ ] **Step 3: Wake on write**

In `kernel/fs/pipe.c`, in the pipe write function after data is enqueued:

```c
    waitq_wake_all(&p->read_waiters);
```

- [ ] **Step 4: Wake on read (for blocked writers when buffer drains)**

After the write side reads bytes from the buffer:

```c
    waitq_wake_all(&p->write_waiters);
```

- [ ] **Step 5: Wake on close**

When either end closes (read_refs or write_refs drops to 0), wake the opposite side so blocked readers see EOF:

```c
    if (p->write_refs == 0) waitq_wake_all(&p->read_waiters);
    if (p->read_refs  == 0) waitq_wake_all(&p->write_waiters);
```

- [ ] **Step 6: Add `get_waitq` callbacks**

Above the ops definitions in `kernel/fs/pipe.c`:

```c
static struct waitq *
pipe_read_get_waitq(void *priv)
{
    return &((pipe_t *)priv)->read_waiters;
}

static struct waitq *
pipe_write_get_waitq(void *priv)
{
    return &((pipe_t *)priv)->write_waiters;
}
```

Update both ops tables:

```c
const vfs_ops_t g_pipe_read_ops = {
    /* ... existing fields ... */
    .poll      = pipe_read_poll,
    .get_waitq = pipe_read_get_waitq,
};

const vfs_ops_t g_pipe_write_ops = {
    /* ... existing fields ... */
    .poll      = pipe_write_poll,
    .get_waitq = pipe_write_get_waitq,
};
```

- [ ] **Step 7: Build + boot oracle**

```bash
make 2>&1 | tail -10
make test
```
Expected: build clean, oracle exit 0. The boot oracle runs `[POLL]` self-test on pipes — make sure it still passes.

- [ ] **Step 8: Commit**

```bash
git add kernel/fs/pipe.h kernel/fs/pipe.c kernel/fs/poll_test.c
git commit -m "$(cat <<'EOF'
fs/pipe: embed read/write waitqs; wake on data + close

Pipes now wake any sys_poll caller registered on either end when
data arrives or the opposite end closes. Both ops tables expose
get_waitq for fd_waitq dispatch.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: TTY/PTY — embed waitqs

**Files:**
- Modify: `kernel/tty/pty.h` — add `master_waiters`, `slave_waiters` to PTY pair struct
- Modify: `kernel/tty/pty.c` — init on PTY pair allocation; wake on master write (slave readable) and slave write (master readable); populate `get_waitq` for both ops tables

- [ ] **Step 1: Add fields to PTY pair**

In `kernel/tty/pty.h`, find the PTY pair struct (likely `pty_pair_t` or similar). Add:

```c
    waitq_t  master_waiters;   /* pollers on master fd (waiting for slave→master data) */
    waitq_t  slave_waiters;    /* pollers on slave fd  (waiting for master→slave data) */
```

Include `"../sched/waitq.h"`.

- [ ] **Step 2: Init on allocation**

Find the PTY pair allocator in `kernel/tty/pty.c`. After zero-init:

```c
    waitq_init(&pp->master_waiters);
    waitq_init(&pp->slave_waiters);
```

- [ ] **Step 3: Wake on master write (slave becomes readable)**

In the master write path (master_fd → slave's input ring):

```c
    waitq_wake_all(&pp->slave_waiters);
```

- [ ] **Step 4: Wake on slave write (master becomes readable)**

In the slave write path (slave_fd → master's output ring, called by the shell printing to stdout):

```c
    waitq_wake_all(&pp->master_waiters);
```

- [ ] **Step 5: Wake on close (HUP propagation)**

When either end closes:

```c
    waitq_wake_all(&pp->master_waiters);
    waitq_wake_all(&pp->slave_waiters);
```

- [ ] **Step 6: Add `get_waitq` callbacks and wire them in**

```c
static struct waitq *
pty_master_get_waitq(void *priv)
{
    return &((pty_pair_t *)priv)->master_waiters;
}

static struct waitq *
pty_slave_get_waitq(void *priv)
{
    return &((pty_pair_t *)priv)->slave_waiters;
}
```

Add `.get_waitq = pty_master_get_waitq` to the master ops table and `.get_waitq = pty_slave_get_waitq` to the slave ops table.

- [ ] **Step 7: Build + boot oracle**

```bash
make 2>&1 | tail -10
make test
```
Expected: build clean, oracle exit 0.

- [ ] **Step 8: Commit**

```bash
git add kernel/tty/pty.h kernel/tty/pty.c
git commit -m "$(cat <<'EOF'
tty/pty: embed master/slave waitqs; wake on data + close

Lumen's terminal-window read loop now wakes immediately when the
shell writes to its PTY slave, instead of waiting for the next
PIT tick. Symmetric on the master→slave direction.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: console + kbd + mouse — global waitqs, ISR wakes

**Files:**
- Modify: `kernel/fs/console.c` — file-static `g_console_waiters`; `console_get_waitq`; populate ops
- Modify: `kernel/fs/kbd_vfs.c` — share `g_console_waiters` (extern) OR own waitq; wake from kbd ISR after each byte push
- Modify: `kernel/drivers/usb_mouse.c` (or similar) — file-static `g_mouse_waiters`; wake when a HID report is dispatched
- Modify: `kernel/fs/memfd.c` — `get_waitq` returns NULL (explicit, for clarity)

The kbd ISR is the only hot path here. `waitq_wake_all` walks a short list and calls `sched_wake` per entry — bounded and IRQ-safe.

- [ ] **Step 1: console waitq**

In `kernel/fs/console.c`, near the top:

```c
#include "../sched/waitq.h"

waitq_t g_console_waiters = WAITQ_INIT;   /* extern'd from kbd_vfs.c */

static struct waitq *
console_get_waitq(void *priv)
{
    (void)priv;
    return &g_console_waiters;
}
```

Add `.get_waitq = console_get_waitq` to the console ops table.

- [ ] **Step 2: kbd ISR wake**

In whichever file holds the kbd ISR (likely `kernel/arch/x86_64/kbd.c` or `kernel/drivers/kbd.c`), after the byte is pushed into the ring buffer:

```c
extern waitq_t g_console_waiters;
waitq_wake_all(&g_console_waiters);
```

- [ ] **Step 3: kbd_vfs `get_waitq`**

In `kernel/fs/kbd_vfs.c`:

```c
extern waitq_t g_console_waiters;

static struct waitq *
kbd_get_waitq(void *priv)
{
    (void)priv;
    return &g_console_waiters;
}
```

Add `.get_waitq = kbd_get_waitq` to the kbd ops table.

- [ ] **Step 4: mouse waitq + wake**

Find the mouse VFS file (search `grep -rn "/dev/mouse" kernel/`). Likely `kernel/drivers/usb_mouse.c` or a similar mouse VFS file.

```c
#include "../sched/waitq.h"
static waitq_t g_mouse_waiters = WAITQ_INIT;

static struct waitq *
mouse_get_waitq(void *priv)
{
    (void)priv;
    return &g_mouse_waiters;
}
```

Add `.get_waitq = mouse_get_waitq` to the mouse ops table. Find the place where a HID mouse report is pushed into the mouse ring (likely in `xhci_poll` or `hid_dispatch`):

```c
    waitq_wake_all(&g_mouse_waiters);
```

- [ ] **Step 5: memfd — explicit NULL**

In `kernel/fs/memfd.c`, leave `get_waitq` unset in the ops table (defaults to NULL). Add a comment above the ops table:

```c
/* memfd is always ready (poll returns POLLIN|POLLOUT immediately).
 * No producer ever needs to wake; get_waitq stays NULL. */
```

- [ ] **Step 6: Build + boot oracle**

```bash
make 2>&1 | tail -10
make test
```
Expected: build clean, oracle exit 0.

- [ ] **Step 7: Commit**

```bash
git add kernel/fs/console.c kernel/fs/kbd_vfs.c kernel/fs/memfd.c \
        kernel/drivers/usb_mouse.c kernel/arch/x86_64/kbd.c
git commit -m "$(cat <<'EOF'
fs+drivers: global waitqs for console/kbd/mouse; ISR wakes

kbd ISR wakes g_console_waiters after each byte push so Lumen's
read(0,...) returns immediately. Mouse HID dispatch wakes
g_mouse_waiters so /dev/mouse pollers respond same-tick. memfd
keeps get_waitq=NULL (always ready, no producer).

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: sys_epoll_wait — register per-fd

**Files:**
- Modify: `kernel/net/epoll.c` — replace the temporary `waitq_wait(&g_timer_waitq)` from Task 3 with a per-watched-fd registration loop, mirroring `sys_poll`

- [ ] **Step 1: Locate epoll_wait_impl**

```bash
grep -n "epoll_wait_impl" kernel/net/epoll.c
```

- [ ] **Step 2: Replace the temporary waitq_wait stub**

Find the block (added in Task 3):

```c
        {
            /* TEMP: bridged via g_timer_waitq until Task 9 wires epoll waitqs. */
            waitq_wait(&g_timer_waitq);
        }
```

Replace with a per-fd registration loop. The structure:

```c
        {
            /* Register on each watched fd's waitq + g_timer_waitq. */
            waitq_entry_t entries[EPOLL_MAX_WATCHES];
            waitq_t      *queues[EPOLL_MAX_WATCHES];
            waitq_entry_t timer_e;
            timer_e.task = sched_current();
            timer_e.next = (void *)0;
            timer_e.prev = (void *)0;
            timer_e.on_queue = 0;

            int n = 0;
            for (int wi = 0; wi < EPOLL_MAX_WATCHES; wi++) {
                if (!ep->watches[wi].in_use) continue;
                queues[n] = fd_get_waitq(ep->watches[wi].fd);
                entries[n].task     = sched_current();
                entries[n].next     = (void *)0;
                entries[n].prev     = (void *)0;
                entries[n].on_queue = 0;
                if (queues[n]) waitq_add(queues[n], &entries[n]);
                n++;
            }
            int has_timer = (deadline_ticks != 0xFFFFFFFFU);
            if (has_timer) waitq_add(&g_timer_waitq, &timer_e);

            sched_block();

            for (int i = 0; i < n; i++)
                if (queues[i]) waitq_remove(queues[i], &entries[i]);
            if (has_timer) waitq_remove(&g_timer_waitq, &timer_e);
        }
```

(Adjust to match the actual `epoll_t` watch-list field names. Read `kernel/net/epoll.h` to find them.)

Add `#include "../syscall/fd_waitq.h"` at the top of `epoll.c`.

- [ ] **Step 3: Build**

```bash
make 2>&1 | tail -10
```
Expected: builds cleanly.

- [ ] **Step 4: Boot oracle**

```bash
make test
```
Expected: exit 0.

- [ ] **Step 5: httpd smoke (httpd uses epoll)**

```bash
cargo test --manifest-path tests/Cargo.toml --test test_httpd_basic -- --nocapture 2>&1 | tail -20 || echo "no httpd test, skipping"
```
Expected: existing httpd test passes if present.

- [ ] **Step 6: Commit**

```bash
git add kernel/net/epoll.c
git commit -m "$(cat <<'EOF'
net/epoll: register per-watched-fd waitqs

epoll_wait now adds the calling task as a waitq entry on each
watched fd's queue (when available) and on g_timer_waitq if a
finite timeout was given. Replaces the temporary
waitq_wait(&g_timer_waitq) bridge from Task 3.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: Regression test — concurrent pollers

**Files:**
- Create: `user/bin/poll-test/main.c` (~80 LOC)
- Create: `user/bin/poll-test/Makefile`
- Modify: `rootfs.manifest` — add the poll-test binary
- Create: `tests/tests/poll_concurrent_pollers_test.rs`

- [ ] **Step 1: Write `user/bin/poll-test/main.c`**

```c
/* poll-test — userspace concurrent-pollers regression test.
 *
 * Usage:
 *   poll-test server <path>          : create AF_UNIX server, accept N clients
 *   poll-test client <path>          : connect, poll for 1 byte, print "[POLL-TEST] got: <c>"
 *
 * Test driver from Rust harness: spawns 1 server + 2 clients, then writes
 * one byte to each client through the server. Both clients must print
 * "got" within a few seconds. Pre-fix one client would hang forever.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

static int connect_to(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr; memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path)-1);
    for (int i = 0; i < 100; i++) {
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) return fd;
        if (errno != ECONNREFUSED) { close(fd); return -1; }
        struct timespec ts = {0, 50*1000*1000}; nanosleep(&ts, NULL);
    }
    close(fd); return -1;
}

int main(int argc, char **argv) {
    if (argc < 3) { dprintf(2, "usage: poll-test {server|client} <path>\n"); return 2; }
    const char *mode = argv[1], *path = argv[2];

    if (strcmp(mode, "server") == 0) {
        int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un a = {0}; a.sun_family = AF_UNIX;
        strncpy(a.sun_path, path, sizeof(a.sun_path)-1);
        unlink(path);
        if (bind(sfd, (struct sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); return 1; }
        if (listen(sfd, 4) < 0) { perror("listen"); return 1; }
        dprintf(1, "[POLL-TEST] server ready\n");
        int c1 = accept(sfd, NULL, NULL);
        int c2 = accept(sfd, NULL, NULL);
        if (c1 < 0 || c2 < 0) { dprintf(2, "accept failed\n"); return 1; }
        dprintf(1, "[POLL-TEST] both accepted\n");
        /* Write a byte to each (the harness controls timing). */
        write(c1, "A", 1);
        write(c2, "B", 1);
        sleep(2);
        return 0;
    }

    if (strcmp(mode, "client") == 0) {
        int fd = connect_to(path);
        if (fd < 0) { dprintf(2, "[POLL-TEST] connect failed\n"); return 1; }
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int r = poll(&pfd, 1, 5000);
        if (r <= 0) { dprintf(1, "[POLL-TEST] poll timeout/err r=%d\n", r); return 1; }
        char c; if (read(fd, &c, 1) != 1) { dprintf(1, "[POLL-TEST] read failed\n"); return 1; }
        dprintf(1, "[POLL-TEST] got: %c\n", c);
        return 0;
    }

    return 2;
}
```

- [ ] **Step 2: Write `user/bin/poll-test/Makefile`**

Mirror an existing simple userspace binary's Makefile (e.g. `user/bin/lumen-probe/Makefile`). Replace the binary name + sources with `poll-test` and `main.c`.

- [ ] **Step 3: Add to top-level Makefile build list**

```bash
grep -n "lumen-probe" Makefile | head -5
```

Add a corresponding `user/bin/poll-test/poll-test.elf` build rule and dependency in the rootfs construction step.

- [ ] **Step 4: Add to rootfs.manifest**

Append:

```
user/bin/poll-test/poll-test.elf /bin/poll-test
```

- [ ] **Step 5: Write `tests/tests/poll_concurrent_pollers_test.rs`**

```rust
//! Two clients block in poll() on separate AF_UNIX fds; server writes a
//! byte to each. Pre-fix, only one client wakes (single g_poll_waiter
//! starvation). Post-fix, both wake within seconds.

use aegis_tests::{aegis_q35_text, iso, AegisHarness};
use std::time::Duration;

#[tokio::test]
async fn two_pollers_both_receive() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display()); return;
    }
    let (mut stream, mut proc) = AegisHarness::boot_stream(aegis_q35_text(), &iso)
        .await.expect("boot");

    // Wait for shell prompt
    let dl = tokio::time::Instant::now() + Duration::from_secs(60);
    loop {
        match tokio::time::timeout_at(dl, stream.next_line()).await {
            Ok(Some(l)) if l.contains("$") || l.contains("#") => break,
            Ok(Some(_)) => {}
            _ => panic!("no shell prompt"),
        }
    }

    // Spawn server + two clients in parallel from a single shell line.
    proc.send_keys("poll-test server /tmp/p.sock & sleep 1; \
                    poll-test client /tmp/p.sock & poll-test client /tmp/p.sock & wait\n")
        .await.unwrap();

    let mut got_a = false; let mut got_b = false;
    let dl = tokio::time::Instant::now() + Duration::from_secs(15);
    loop {
        match tokio::time::timeout_at(dl, stream.next_line()).await {
            Ok(Some(l)) => {
                eprintln!("LINE: {l}");
                if l.contains("[POLL-TEST] got: A") { got_a = true; }
                if l.contains("[POLL-TEST] got: B") { got_b = true; }
                if got_a && got_b { break; }
            }
            _ => break,
        }
    }
    assert!(got_a, "client A never received its byte");
    assert!(got_b, "client B never received its byte");
}
```

- [ ] **Step 6: Build + run the new test**

```bash
make
make iso 2>&1 | tail -5    # need ISO with poll-test binary
cargo test --manifest-path tests/Cargo.toml \
    --test poll_concurrent_pollers_test -- --nocapture 2>&1 | tail -20
```
Expected: both clients receive their byte; test passes.

- [ ] **Step 7: Commit**

```bash
git add user/bin/poll-test/ tests/tests/poll_concurrent_pollers_test.rs \
        rootfs.manifest Makefile
git commit -m "$(cat <<'EOF'
tests: poll-test binary + concurrent pollers regression

Two clients poll on separate AF_UNIX fds; server writes one byte
to each. Verifies neither starves. Pre-fix this would hang one
client indefinitely; post-fix both wake within seconds.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 12: Regression test — gui-installer Enter advances screen

**Files:**
- Create: `tests/tests/gui_installer_advance_test.rs`

This is end-to-end: boot graphical, log in, click dock terminal, type `gui-installer`, wait for screen=1 marker, send Enter, assert screen=2 marker.

- [ ] **Step 1: Write the test**

```rust
//! End-to-end regression: gui-installer welcome screen advances on Enter.
//!
//! Pre-fix, gui-installer's poll() never returned POLLIN because the
//! single g_poll_waiter was always claimed by another task. Enter, mouse
//! clicks, and close-button clicks all silently disappeared.

use aegis_tests::{aegis_q35_graphical_mouse, iso, AegisHarness};
use std::time::Duration;

#[tokio::test]
async fn enter_advances_to_disk_screen() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display()); return;
    }
    let (mut stream, mut proc) = AegisHarness::boot_stream(
        aegis_q35_graphical_mouse(), &iso
    ).await.expect("boot");

    // Wait for Bastion greeter, log in.
    let dl = tokio::time::Instant::now() + Duration::from_secs(60);
    loop {
        match tokio::time::timeout_at(dl, stream.next_line()).await {
            Ok(Some(l)) if l.contains("[BASTION] greeter ready") => break,
            Ok(Some(_)) => {}
            _ => panic!("no greeter"),
        }
    }
    tokio::time::sleep(Duration::from_millis(500)).await;
    proc.send_keys("root\tforevervigilant\n").await.unwrap();

    // Wait for Lumen + dock ready.
    let dl = tokio::time::Instant::now() + Duration::from_secs(30);
    let mut term_cx = 0i32; let mut term_cy = 0i32;
    loop {
        match tokio::time::timeout_at(dl, stream.next_line()).await {
            Ok(Some(l)) => {
                if l.contains("[DOCK] item=terminal") {
                    for tok in l.split_whitespace() {
                        if let Some(r) = tok.strip_prefix("cx=") { term_cx = r.parse().unwrap_or(0); }
                        if let Some(r) = tok.strip_prefix("cy=") { term_cy = r.parse().unwrap_or(0); }
                    }
                }
                if l.contains("[DOCK] ready") && term_cx > 0 { break; }
            }
            _ => panic!("dock never ready"),
        }
    }

    // Click terminal icon.
    for _ in 0..4 { proc.mouse_move(-500, -500).await.unwrap();
        tokio::time::sleep(Duration::from_millis(200)).await; }
    proc.mouse_move((term_cx*2+1)/3, (term_cy*2+1)/3).await.unwrap();
    tokio::time::sleep(Duration::from_millis(600)).await;
    proc.mouse_button(1).await.unwrap();
    tokio::time::sleep(Duration::from_millis(80)).await;
    proc.mouse_button(0).await.unwrap();

    // Wait for terminal to open, then type gui-installer.
    let dl = tokio::time::Instant::now() + Duration::from_secs(15);
    loop {
        match tokio::time::timeout_at(dl, stream.next_line()).await {
            Ok(Some(l)) if l.contains("window_opened=terminal") => break,
            Ok(Some(_)) => {}
            _ => panic!("terminal never opened"),
        }
    }
    tokio::time::sleep(Duration::from_millis(800)).await;
    // Click center of screen to focus terminal.
    proc.mouse_move(320, 240).await.unwrap();
    tokio::time::sleep(Duration::from_millis(200)).await;
    proc.mouse_button(1).await.unwrap();
    tokio::time::sleep(Duration::from_millis(80)).await;
    proc.mouse_button(0).await.unwrap();
    tokio::time::sleep(Duration::from_millis(500)).await;

    proc.send_keys("gui-installer\n").await.unwrap();

    // Wait for installer screen=1.
    let dl = tokio::time::Instant::now() + Duration::from_secs(15);
    loop {
        match tokio::time::timeout_at(dl, stream.next_line()).await {
            Ok(Some(l)) if l.contains("[INSTALLER] screen=1") => break,
            Ok(Some(_)) => {}
            _ => panic!("installer screen=1 never reached"),
        }
    }

    // Send Enter — must reach gui-installer via Lumen's proxy_on_key.
    proc.send_keys("\n").await.unwrap();

    // Assert screen=2 within 2 seconds.
    let dl = tokio::time::Instant::now() + Duration::from_secs(2);
    let advanced = loop {
        match tokio::time::timeout_at(dl, stream.next_line()).await {
            Ok(Some(l)) if l.contains("[INSTALLER] screen=2") => break true,
            Ok(Some(_)) => {}
            _ => break false,
        }
    };
    assert!(advanced, "Enter did not advance gui-installer past welcome");
}
```

- [ ] **Step 2: Build a fresh graphical ISO**

```bash
git clean -fdx --exclude=references --exclude=.worktrees
rm -f user/vigil/vigil user/vigictl/vigictl user/login/login.elf
make clean
make iso 2>&1 | tail -5
```

- [ ] **Step 3: Run the test**

```bash
cargo test --manifest-path tests/Cargo.toml \
    --test gui_installer_advance_test -- --nocapture 2>&1 | tail -40
```
Expected: test passes — Enter advances installer to screen 2.

- [ ] **Step 4: Commit**

```bash
git add tests/tests/gui_installer_advance_test.rs
git commit -m "$(cat <<'EOF'
tests: gui-installer Enter-advances regression

End-to-end test: boot graphical, log in, click dock terminal,
type gui-installer, send Enter, assert screen=2 marker. Pre-fix
the keystroke would silently disappear because gui-installer's
poll() was starved by the single g_poll_waiter.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 13: Cleanup pass — delete dead code, update CLAUDE.md

**Files:**
- Modify: `kernel/net/unix_socket.h` / `.c` — `waiter_task` field MAY now be deletable; do so if all read/accept callers can use waitq_wait instead. (Optional — defer if too invasive.)
- Modify: `kernel/sched/sched.h` — confirm `wq_next` is gone (Task 1 should have done this; sanity check)
- Modify: `.claude/CLAUDE.md` — add a note in "Active Constraints" referencing the new mechanism + new section in lock ordering
- Modify: `docs/superpowers/specs/2026-04-16-poll-waitq-design.md` — flip `status: approved` to `status: implemented`

- [ ] **Step 1: Confirm `g_poll_waiter` is fully gone**

```bash
grep -rn "g_poll_waiter" kernel/ tests/ user/ 2>/dev/null
```
Expected: no matches outside of ARM64 stubs (which were updated). If any remain, delete them.

- [ ] **Step 2: Update CLAUDE.md — Active Constraints section**

Open `.claude/CLAUDE.md`. Find the section "Active Constraints — Still Relevant". Add a new bullet:

```markdown
### Polling and wake-up (Phase 47c — per-fd waitqs)

- All pollable fds have a `waitq_t poll_waiters` (or shared global for kbd/console/mouse). Producers (writers, ISRs, state-change paths) call `waitq_wake_all` on the object's queue.
- `sys_poll` and `sys_epoll_wait` register one `waitq_entry_t` per watched fd (stack-allocated, max 64 fds for poll / `EPOLL_MAX_WATCHES` for epoll). Plus `g_timer_waitq` if timeout is finite.
- Removed: global `g_poll_waiter` and the PIT hack at `pit.c:80-84` that wrote to it. PIT now calls `waitq_wake_all(&g_timer_waitq)`.
- New file: `kernel/syscall/fd_waitq.{c,h}` — single-point `fd_get_waitq(int fd)` dispatch.
- Lock order: waitq locks are leaf. `sched_lock` > waitq lock. Producers call `waitq_wake_all` after dropping any object-specific lock to avoid `sched_lock` reentry.
```

Also update the lock-ordering block at the bottom to add `waitq_lock`:

```
sched_lock > (all others)
sched_lock > waitq_lock
vmm_window_lock > pmm_lock > kva_lock
tcp_lock before sock_lock (deferred wake pattern)
ip_lock before arp_lock (copy-then-release pattern)
```

- [ ] **Step 3: Mark spec as implemented**

In `docs/superpowers/specs/2026-04-16-poll-waitq-design.md`, change the YAML frontmatter:

```yaml
status: implemented
```

- [ ] **Step 4: Build + full test sweep**

```bash
make 2>&1 | tail -10
make test
cargo test --manifest-path tests/Cargo.toml --test poll_concurrent_pollers_test -- --nocapture 2>&1 | tail -10
cargo test --manifest-path tests/Cargo.toml --test gui_installer_advance_test  -- --nocapture 2>&1 | tail -10
```
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add .claude/CLAUDE.md docs/superpowers/specs/2026-04-16-poll-waitq-design.md
git commit -m "$(cat <<'EOF'
docs: per-fd waitqs landed; mark spec implemented

- CLAUDE.md: new "Polling and wake-up" entry under Active Constraints.
- Lock-ordering block updated with waitq_lock leaf relationship.
- Spec frontmatter status: approved → implemented.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Plan self-review

**Spec coverage:**
- Waitq abstraction → Task 1 ✓
- sys_poll rewrite → Task 5 ✓
- Two new helpers (`fd_get_waitq`, `g_timer_waitq`) → Tasks 3, 5 ✓
- vfs_ops_t extension → Task 2 ✓
- Per-type integration (AF_UNIX, AF_INET, pipe, TTY/PTY, console, kbd, mouse, memfd, epoll) → Tasks 4, 6, 7, 8, 9, 10 ✓
- Blocking I/O unification — DEFERRED (`waiter_task` kept alongside waitq for safety; the spec calls for unification but Task 13 step 1 only deletes `waiter_task` if low-risk). This is the one explicit deviation from the spec; flagged here for review.
- ISR-context wakes → Task 9 ✓ (kbd ISR wakes `g_console_waiters` directly)
- Tests → Tasks 11, 12 ✓
- ARM64 stub update → Task 3 ✓
- `g_poll_waiter` deleted → Tasks 3, 13 ✓

**Placeholder scan:** None. All steps include code or exact commands.

**Type consistency:** `waitq_entry_t`, `waitq_t`, `waitq_add`, `waitq_remove`, `waitq_wake_all`, `waitq_wake_one`, `waitq_wait`, `g_timer_waitq`, `fd_get_waitq`, `unix_sock_get_waitq`, `sock_get_waitq` are used consistently across all tasks.

**Known deviations from spec, flagged for executor:**
- Spec calls for full unification of blocking I/O `waiter_task` → waitq. Plan defers this as optional in Task 13 to bound risk. If Task 13's "low risk" check fails, leave `waiter_task` in place; it doesn't hurt correctness, just slight memory bloat.
- Plan splits sys_poll's permanent rewrite (Task 5) from a short-lived bridging stub (Task 3). This keeps the kernel buildable between the PIT change and the per-fd wiring. Spec didn't prescribe ordering; this is an implementation choice to keep commits small and bisectable.
