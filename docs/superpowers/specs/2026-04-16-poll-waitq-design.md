---
title: Per-fd wait queues — replace global g_poll_waiter
date: 2026-04-16
status: approved
author: Dylan Hart (with Claude)
---

# Per-fd wait queues — replace global `g_poll_waiter`

## Problem

`kernel/arch/x86_64/pit.c:46` defines a single global `g_poll_waiter`. `sys_poll`
and `sys_epoll_wait` both write their current task into it before
`sched_block()`. The PIT handler wakes only that one task each tick. Writes to
AF_UNIX, TCP, pipes, etc. wake their own per-object `waiter_task` pointer used
by blocking reads — they do **not** wake `g_poll_waiter`.

Consequences:

- Two tasks blocked in `sys_poll` simultaneously: only the most recent setter
  ever wakes from PIT. The other starves indefinitely.
- A task blocked in poll never wakes when data actually arrives — only when the
  PIT fires. Latency floor of one PIT tick (10 ms).
- Symptom hit by gui-installer: dock and gui-installer both sit in
  `lumen_wait_event` → `poll`; gui-installer's writes from Lumen accumulate in
  the AF_UNIX ring but `poll` never returns POLLIN. Enter, Next, close-button —
  all silently dropped.

## Goal

Replace the global with per-fd wait queues. Writers wake everyone watching the
specific object. `g_poll_waiter` deleted entirely.

## Non-goals

- Edge-triggered epoll. Level-triggered only, matching current behavior.
- Wait morphing / priority inheritance.
- SMP fairness beyond "wake everyone, scheduler picks order."
- New userspace API. `poll`, `select` (still ENOSYS), `epoll_wait` keep their
  existing semantics.

## Architecture

### Wait queue primitive

New file: `kernel/sched/waitq.{c,h}`.

```c
typedef struct waitq_entry {
    struct aegis_task_t *task;
    struct waitq_entry  *next;
    struct waitq_entry  *prev;
} waitq_entry_t;

typedef struct {
    waitq_entry_t *head;
    spinlock_t     lock;
} waitq_t;

#define WAITQ_INIT { (void *)0, SPINLOCK_INIT }

void waitq_add(waitq_t *q, waitq_entry_t *e);
void waitq_remove(waitq_t *q, waitq_entry_t *e);
void waitq_wake_all(waitq_t *q);
void waitq_wake_one(waitq_t *q);
```

Properties:

- Caller owns entry storage. Typical use is a stack array sized to `nfds`
  inside the syscall frame.
- `waitq_remove` is idempotent — safe to call on an entry that was never added
  or has already been removed.
- All four functions take `spin_lock_irqsave(&q->lock, ...)`. Safe to call
  from ISR context.
- `wake_*` does not remove entries; the woken task removes itself after
  resuming. This avoids a use-after-free if the wake races with the unblock.
- Lock order: waitq locks are leaf locks. They may be acquired with no other
  locks held except `sched_lock` (which `sched_wake` acquires internally).

### sys_poll rewrite

`kernel/syscall/sys_socket.c`. Pseudocode (real impl handles `copy_from_user`
and per-fd dispatch):

```c
uint64_t sys_poll(uint64_t fds_ptr, uint64_t nfds, uint64_t timeout_ms) {
    if (nfds > 64) return -EINVAL;

    k_pollfd_t fds[64];
    copy_from_user(fds, fds_ptr, nfds * sizeof(k_pollfd_t));

    waitq_entry_t fd_entries[64];
    waitq_t      *fd_queues[64];
    waitq_entry_t timer_entry = { .task = sched_current() };

    uint64_t now = arch_get_ticks();
    uint64_t deadline = (timeout_ms == (uint64_t)-1) ? 0 :
                         (timeout_ms == 0)           ? now :
                                                       now + (timeout_ms / 10);

    for (;;) {
        int ready = poll_iterate_fds(fds, nfds);
        if (ready > 0 || timeout_ms == 0) {
            copy_to_user(fds_ptr, fds, nfds * sizeof(k_pollfd_t));
            return ready;
        }
        if (deadline && arch_get_ticks() >= deadline) {
            copy_to_user(fds_ptr, fds, nfds * sizeof(k_pollfd_t));
            return 0;
        }

        for (uint64_t i = 0; i < nfds; i++) {
            fd_queues[i] = fd_get_waitq(fds[i].fd);
            if (fd_queues[i]) {
                fd_entries[i].task = sched_current();
                waitq_add(fd_queues[i], &fd_entries[i]);
            }
        }
        if (deadline) waitq_add(&g_timer_waitq, &timer_entry);

        sched_block();

        for (uint64_t i = 0; i < nfds; i++)
            if (fd_queues[i]) waitq_remove(fd_queues[i], &fd_entries[i]);
        if (deadline) waitq_remove(&g_timer_waitq, &timer_entry);
    }
}
```

### Two new helpers

**`fd_get_waitq(int fd)`** in `kernel/syscall/fd_waitq.c`:

```c
waitq_t *fd_get_waitq(int fd) {
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    /* AF_INET socket */
    uint32_t sid = sock_id_from_fd(fd, proc);
    if (sid != SOCK_NONE) return sock_get_waitq(sid);
    /* AF_UNIX socket */
    uint32_t uid = unix_sock_id_from_fd(fd, proc);
    if (uid != UNIX_NONE) return unix_sock_get_waitq(uid);
    /* VFS fd — pipe, tty, console, kbd, mouse, memfd */
    if (fd >= 0 && fd < PROC_MAX_FDS && proc->fd_table->fds[fd].ops) {
        const vfs_ops_t *ops = proc->fd_table->fds[fd].ops;
        if (ops->get_waitq)
            return ops->get_waitq(proc->fd_table->fds[fd].priv);
    }
    return NULL;  /* permissive default in poll_iterate_fds */
}
```

**`g_timer_waitq`** declared in `kernel/sched/waitq.c`:

```c
waitq_t g_timer_waitq = WAITQ_INIT;
```

PIT (`kernel/arch/x86_64/pit.c`) replaces the `g_poll_waiter` block with:

```c
waitq_wake_all(&g_timer_waitq);
```

The `inb(0x61)` SLIRP yield stays — unrelated to poll.

### vfs_ops_t extension

`kernel/fs/vfs.h`:

```c
typedef struct {
    int      (*read)(...);
    int      (*write)(...);
    void     (*close)(...);
    /* ... existing fields ... */
    uint16_t (*poll)(void *priv);
    waitq_t *(*get_waitq)(void *priv);   /* NEW — optional, NULL = no waitq */
} vfs_ops_t;
```

Types implementing both `poll` and `get_waitq`: pipe, tty, console (kbd input
side), mouse, AF_UNIX. `memfd` provides `poll` (always ready) but no
`get_waitq` (no events ever fire).

### Per-type integration

| Type                     | waitq location              | Producers (call `waitq_wake_all`)                                              |
|--------------------------|-----------------------------|--------------------------------------------------------------------------------|
| AF_UNIX (`unix_sock_t`)  | new `poll_waiters` field    | `unix_sock_write`, `unix_sock_connect` (wakes listener), `unix_sock_free` (HUP) |
| AF_INET (`sock_t`)       | new `poll_waiters` field    | TCP rx, accept enqueue, UDP rx, TCP state→CLOSE_WAIT/CLOSED                    |
| pipe (`pipe_t`)          | one per direction           | `pipe_write` (wake reader), `pipe_close` (EOF)                                 |
| TTY/PTY (`pty_pair_t`)   | master_waitq + slave_waitq  | line-discipline input → master_waitq; PTY master write → slave_waitq           |
| console (`/dev/console`) | global static               | kbd ISR after ring push                                                        |
| kbd (`/dev/kbd`)         | shared with console         | same path as console                                                            |
| mouse (`/dev/mouse`)     | global static               | xhci HID report dispatch / mouse poll                                          |
| memfd                    | none (`get_waitq` → NULL)   | n/a                                                                            |
| epoll                    | epoll instance has waitq    | producer wakes any blocked epoll_wait when a watched fd becomes ready          |

### Blocking I/O unification

`unix_sock_read`, `pipe_read`, `tcp_recv`, etc. currently set
`obj->waiter_task = sched_current()` then `sched_block()`. They migrate to use
the same waitq via stack-allocated entry:

```c
waitq_entry_t e = { .task = sched_current() };
waitq_add(&us->poll_waiters, &e);
sched_block();
waitq_remove(&us->poll_waiters, &e);
```

Writers only need to call `waitq_wake_all` once — both blocked readers and
poll'd readers wake together. Old `waiter_task` field deleted from each object.

### sys_epoll_wait

Same shape as `sys_poll`. `epoll_wait_impl` uses `g_poll_waiter` today; it
switches to per-fd waitqs over the watched fd set + the timer waitq. Single
`waitq_t` per epoll instance is unnecessary — epoll_wait blocks only on the
fds it watches.

### ISR-context wakes

kbd, mouse, NIC ISRs call `waitq_wake_all` directly. Safe because:

- `waitq_wake_all` uses `spin_lock_irqsave`.
- `sched_wake` already supports being called from ISR context (PIT does this
  today via `g_poll_waiter`).
- Wake operation is bounded: walk the (short) waitq list, call `sched_wake`
  per entry. List length is bounded by number of poll'ers on that object;
  typically ≤ 4.

## Data flow

Lumen writes a `LUMEN_EV_KEY` event to gui-installer:

1. `proxy_on_key` → `write(client_fd, ...)` → kernel `unix_sock_write`.
2. `unix_sock_write` puts bytes in client's read ring under `unix_lock`.
3. While still under the lock, calls `waitq_wake_all(&peer->poll_waiters)`.
4. gui-installer's `sys_poll` task is in that waitq → `sched_wake` → task
   becomes RUNNABLE.
5. On next context switch (immediate if higher priority, otherwise next
   `sched_tick`), gui-installer resumes. `sched_block` returns.
6. gui-installer cleanup: `waitq_remove` from each registered waitq.
7. Loop iterates: `poll_iterate_fds` → `unix_vfs_poll` → POLLIN → return 1.
8. gui-installer calls `recv_event`, processes Enter, advances to disk screen.

Latency: dominated by scheduler context-switch (microseconds), not PIT tick.

## Error handling

- `nfds > 64` → `EINVAL`. (Same as today.)
- Bad `fds_ptr` → `EFAULT`. (Same as today.)
- Bad fd in pollfd array → `revents = POLLNVAL`. (Same as today.)
- Signal interrupts `sched_block` — task wakes, cleanup unregisters from all
  queues, syscall returns `EINTR`. (Today's `sched_block` does not yet support
  signal-driven wake; if signals during poll are not currently handled, this
  spec preserves that behavior. Adding signal interruption is a follow-up.)

## Testing

Two new userspace integration tests under `tests/tests/`:

1. **`poll_concurrent_pollers_test.rs`** — Boots Aegis, spawns two test
   binaries that both `poll()` on AF_UNIX sockets. A third binary writes to
   each socket. Verifies both pollers receive POLLIN within 100 ms of the
   write. Today this would hang one of them indefinitely.

2. **`gui_installer_advance_test.rs`** — Boots graphical mode, logs in via
   Bastion, opens terminal via dock click, types `gui-installer`, waits for
   `[INSTALLER] screen=1` marker, sends Enter, asserts `[INSTALLER] screen=2`
   appears within 2 seconds. End-to-end regression for the original bug.

Boot-oracle (`tests/expected/boot.txt`) does not change — poll behavior is
not visible during init.

Build: `make` must produce a kernel with no new warnings (`-Werror` in CFLAGS).
ARM64 stub at `kernel/arch/arm64/stubs.c:204` needs corresponding update —
delete `g_poll_waiter` stub, add `waitq_t g_timer_waitq` stub if needed.

## Migration / rollout

Single PR. No feature flag. Old `g_poll_waiter` and per-object `waiter_task`
fields deleted in the same change set. Backward compatibility is not a concern
inside the kernel.

## Risks

- **Lock-order regressions.** waitq locks are leaf, but reviewers must verify
  no caller takes `unix_lock` (or other object locks) while inside
  `waitq_wake_all`. Mitigation: invariant documented in waitq.h, code review
  checks each producer.
- **ISR latency.** ISRs walk the waitq list and call `sched_wake` per entry.
  With ≤4 waiters this is bounded but worth measuring.
- **Memory layout churn.** Adding `waitq_t poll_waiters` to `unix_sock_t`,
  `sock_t`, `pipe_t`, etc. grows static arrays. `unix_sock_t` static array
  size: 32 × current_size + 32 × sizeof(waitq_t) ≈ 32 × 24 = 768 extra bytes.
  Negligible.
- **Unified blocking I/O migration.** Touching `unix_sock_read`, `pipe_read`,
  `tcp_conn_recv` is the main correctness risk. Each migration is small and
  testable in isolation.

## Out of scope (future work)

- Signal-driven `sched_block` wake (EINTR support across all blocking syscalls).
- Per-CPU run queues with affinity-aware wake.
- Edge-triggered epoll.
- Adaptive spin in `waitq_wake_all` for ultra-low latency.
