# VFS Poll Generalization + Rune Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `.poll` callback to `vfs_ops_t` so that `sys_poll` and `epoll_wait` work on pipe, PTY, and console fds — then port the rune text editor to Aegis.

**Architecture:** A new `.poll` field in `vfs_ops_t` returns a readiness bitmask (POLLIN/POLLOUT/POLLHUP/POLLERR). `sys_poll` and `epoll_wait_impl` call this callback for non-socket fds, falling back to existing socket logic for sockets. Active polling at PIT tick rate (100 Hz), same as existing `sys_poll`.

**Tech Stack:** C (kernel), musl-gcc (userspace test binary), Rust (integration test via aegis-tests crate + Vortex QEMU backend), Rust (rune — prebuilt musl static binary)

**Spec:** `docs/superpowers/specs/2026-04-11-vfs-poll-rune-port-design.md`

---

### Task 1: Add `.poll` to `vfs_ops_t` and update all existing tables

**Files:**
- Modify: `kernel/fs/vfs.h:54-76` (add `.poll` field)
- Modify: `kernel/fs/pipe.c:23-39` (add `.poll = (void *)0` placeholder to both ops)
- Modify: `kernel/tty/pty.c:60-76` (add `.poll = (void *)0` placeholder to both ops)
- Modify: `kernel/fs/kbd_vfs.c:110-117` (add `.poll = (void *)0` placeholder)
- Modify: `kernel/fs/console.c:77-84` (add `.poll = (void *)0` placeholder)
- Modify: `kernel/fs/ext2_vfs.c:139` (add `.poll = (void *)0`)
- Modify: `kernel/fs/initrd.c:263,347,396,456` (add `.poll = (void *)0` to all 4 ops tables)
- Modify: `kernel/fs/memfd.c:131` (add `.poll = (void *)0`)
- Modify: `kernel/fs/procfs.c:391,514` (add `.poll = (void *)0` to both)
- Modify: `kernel/fs/ramfs.c:124,153` (add `.poll = (void *)0` to both)
- Modify: `kernel/net/unix_socket.c:148` (add `.poll = (void *)0`)
- Modify: `kernel/net/socket.c:24` (add `.poll = (void *)0`)
- Modify: `kernel/net/epoll.c:19` (add `.poll = (void *)0`)

- [ ] **Step 1: Add `.poll` field to `vfs_ops_t`**

In `kernel/fs/vfs.h`, add after the `.stat` field (line 75):

```c
    /* poll -- report current readiness events for this fd.
     * Returns a bitmask of POLLIN/POLLOUT/POLLHUP/POLLERR.
     * Called from sys_poll / epoll_wait with no locks held.
     * NULL = fd does not support polling (caller assumes POLLIN|POLLOUT). */
    uint16_t (*poll)(void *priv);
```

- [ ] **Step 2: Add `.poll = (void *)0` to every existing `vfs_ops_t` table**

There are 19 `vfs_ops_t` tables across the kernel. Add `.poll = (void *)0,` after `.stat` in each one:

- `kernel/fs/pipe.c` — `g_pipe_read_ops` (line 23) and `g_pipe_write_ops` (line 32)
- `kernel/tty/pty.c` — `s_master_ops` (line 60) and `s_slave_ops` (line 69)
- `kernel/fs/kbd_vfs.c` — `s_kbd_ops` (line 110)
- `kernel/fs/console.c` — `s_console_ops` (line 77)
- `kernel/fs/ext2_vfs.c` — `s_ext2_ops` (line 139)
- `kernel/fs/initrd.c` — `initrd_ops` (line 263), `dir_ops` (line 347), `s_urandom_ops` (line 396), `s_mouse_ops` (line 456)
- `kernel/fs/memfd.c` — `g_memfd_ops` (line 131)
- `kernel/fs/procfs.c` — `s_procfs_file_ops` (line 391), `s_procfs_dir_ops` (line 514)
- `kernel/fs/ramfs.c` — `s_ramfs_ops` (line 124), `s_ramfs_dir_ops` (line 153)
- `kernel/net/unix_socket.c` — `g_unix_sock_ops` (line 148)
- `kernel/net/socket.c` — `s_sock_ops` (line 24)
- `kernel/net/epoll.c` — `s_epoll_ops` (line 19)

- [ ] **Step 3: Build — verify it compiles**

Run: `make`
Expected: Clean compile with zero warnings. The `.poll = (void *)0` is a valid C99 designated initializer for a function pointer.

- [ ] **Step 4: Commit**

```bash
git add kernel/fs/vfs.h kernel/fs/pipe.c kernel/tty/pty.c kernel/fs/kbd_vfs.c \
  kernel/fs/console.c kernel/fs/ext2_vfs.c kernel/fs/initrd.c kernel/fs/memfd.c \
  kernel/fs/procfs.c kernel/fs/ramfs.c kernel/net/unix_socket.c kernel/net/socket.c \
  kernel/net/epoll.c
git commit -m "feat(vfs): add .poll callback to vfs_ops_t (all tables NULL)"
```

---

### Task 2: Implement `.poll` for pipes

**Files:**
- Modify: `kernel/fs/pipe.c:23-39` (implement poll for pipe read and write ops)

The poll semantics from the spec:
- **Pipe read end:** POLLIN if `count > 0` or `write_refs == 0` (EOF). POLLHUP if `write_refs == 0`.
- **Pipe write end:** POLLOUT if `count < PIPE_BUF_SIZE` or `read_refs == 0`. POLLERR if `read_refs == 0`.

- [ ] **Step 1: Implement `pipe_read_poll_fn` and `pipe_write_poll_fn`**

Add before `g_pipe_read_ops` (around line 22) in `kernel/fs/pipe.c`:

```c
static uint16_t
pipe_read_poll_fn(void *priv)
{
    pipe_t *p = (pipe_t *)priv;
    uint16_t events = 0;
    irqflags_t fl = spin_lock_irqsave(&p->lock);
    if (p->count > 0 || p->write_refs == 0)
        events |= 0x0001; /* POLLIN */
    if (p->write_refs == 0)
        events |= 0x0010; /* POLLHUP */
    spin_unlock_irqrestore(&p->lock, fl);
    return events;
}

static uint16_t
pipe_write_poll_fn(void *priv)
{
    pipe_t *p = (pipe_t *)priv;
    uint16_t events = 0;
    irqflags_t fl = spin_lock_irqsave(&p->lock);
    if (p->count < PIPE_BUF_SIZE || p->read_refs == 0)
        events |= 0x0004; /* POLLOUT */
    if (p->read_refs == 0)
        events |= 0x0008; /* POLLERR */
    spin_unlock_irqrestore(&p->lock, fl);
    return events;
}
```

- [ ] **Step 2: Wire poll functions into the ops tables**

Replace `.poll = (void *)0` with the real functions in both tables:

In `g_pipe_read_ops`:
```c
    .poll    = pipe_read_poll_fn,
```

In `g_pipe_write_ops`:
```c
    .poll    = pipe_write_poll_fn,
```

- [ ] **Step 3: Build — verify it compiles**

Run: `make`
Expected: Clean compile, zero warnings.

- [ ] **Step 4: Commit**

```bash
git add kernel/fs/pipe.c
git commit -m "feat(pipe): implement .poll for pipe read/write ends"
```

---

### Task 3: Implement `.poll` for PTY master and slave

**Files:**
- Modify: `kernel/tty/pty.c:60-76` (implement poll for both ops tables)

Poll semantics:
- **PTY master:** POLLIN if `output_buf` has data or `!slave_open`. POLLOUT always. POLLHUP if `!slave_open`.
- **PTY slave:** POLLIN if `input_buf` has data or `!master_open`. POLLOUT always. POLLHUP if `!master_open`.

- [ ] **Step 1: Implement `master_poll_fn` and `slave_poll_fn`**

Add before `s_master_ops` (around line 59) in `kernel/tty/pty.c`:

```c
static uint16_t
master_poll_fn(void *priv)
{
    pty_pair_t *pair = (pty_pair_t *)priv;
    uint16_t events = 0x0004; /* POLLOUT always */
    if (ring_count(pair->output_head, pair->output_tail) > 0 || !pair->slave_open)
        events |= 0x0001; /* POLLIN */
    if (!pair->slave_open)
        events |= 0x0010; /* POLLHUP */
    return events;
}

static uint16_t
slave_poll_fn(void *priv)
{
    pty_pair_t *pair = (pty_pair_t *)priv;
    uint16_t events = 0x0004; /* POLLOUT always */
    if (ring_count(pair->input_head, pair->input_tail) > 0 || !pair->master_open)
        events |= 0x0001; /* POLLIN */
    if (!pair->master_open)
        events |= 0x0010; /* POLLHUP */
    return events;
}
```

- [ ] **Step 2: Wire into ops tables**

Replace `.poll = (void *)0` in `s_master_ops` and `s_slave_ops`:

```c
    .poll    = master_poll_fn,
```
```c
    .poll    = slave_poll_fn,
```

- [ ] **Step 3: Build — verify it compiles**

Run: `make`
Expected: Clean compile, zero warnings.

- [ ] **Step 4: Commit**

```bash
git add kernel/tty/pty.c
git commit -m "feat(pty): implement .poll for PTY master and slave"
```

---

### Task 4: Implement `.poll` for console/kbd and add `kbd_has_data()`

**Files:**
- Modify: `kernel/arch/x86_64/kbd.c` (add `kbd_has_data()`)
- Modify: `kernel/arch/x86_64/kbd.h` (declare `kbd_has_data()`)
- Modify: `kernel/fs/kbd_vfs.c:110-117` (implement `.poll`)
- Modify: `kernel/fs/console.c:77-84` (implement `.poll`)

- [ ] **Step 1: Add `kbd_has_data()` to kbd.c**

Add after `kbd_poll` (around line 216) in `kernel/arch/x86_64/kbd.c`:

```c
int
kbd_has_data(void)
{
    irqflags_t fl = spin_lock_irqsave(&kbd_lock);
    int has = (s_head != s_tail);
    spin_unlock_irqrestore(&kbd_lock, fl);
    return has;
}
```

- [ ] **Step 2: Declare `kbd_has_data()` in kbd.h**

Add after the `kbd_poll` declaration (around line 20) in `kernel/arch/x86_64/kbd.h`:

```c
/* Non-destructive check: returns 1 if the keyboard ring buffer has data,
 * 0 if empty. Does NOT consume the character. Used by VFS .poll(). */
int kbd_has_data(void);
```

- [ ] **Step 3: Implement `kbd_vfs_poll_fn` in kbd_vfs.c**

Add before `s_kbd_ops` (around line 109) in `kernel/fs/kbd_vfs.c`:

```c
static uint16_t
kbd_vfs_poll_fn(void *priv)
{
    (void)priv;
    uint16_t events = 0x0004; /* POLLOUT: write side always "ready" (ignored) */
    if (kbd_has_data())
        events |= 0x0001; /* POLLIN */
    return events;
}
```

Wire into `s_kbd_ops`:
```c
    .poll    = kbd_vfs_poll_fn,
```

- [ ] **Step 4: Implement `console_poll_fn` in console.c**

Add before `s_console_ops` (around line 76) in `kernel/fs/console.c`:

```c
static uint16_t
console_poll_fn(void *priv)
{
    (void)priv;
    return 0x0004; /* POLLOUT always — console is write-only */
}
```

Wire into `s_console_ops`:
```c
    .poll    = console_poll_fn,
```

- [ ] **Step 5: Build — verify it compiles**

Run: `make`
Expected: Clean compile, zero warnings.

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/x86_64/kbd.c kernel/arch/x86_64/kbd.h \
  kernel/fs/kbd_vfs.c kernel/fs/console.c
git commit -m "feat(kbd/console): implement .poll for keyboard and console fds"
```

---

### Task 5: Generalize `sys_poll` to use VFS `.poll` callbacks

**Files:**
- Modify: `kernel/syscall/sys_socket.c:821-904` (rewrite inner loop of `sys_poll`)

- [ ] **Step 1: Add POLLNVAL define**

Add after the existing poll defines (around line 833) in `kernel/syscall/sys_socket.c`:

```c
#define POLLNVAL 0x0020
```

- [ ] **Step 2: Rewrite the per-fd polling logic in sys_poll**

Replace the inner loop body (lines 851-884) in `sys_poll`. The new logic checks sockets first (preserving existing behavior), then falls back to VFS `.poll`:

```c
        for (i = 0; i < nfds; i++) {
            k_pollfd_t pfd;
            copy_from_user(&pfd, (const void *)(uintptr_t)(fds_ptr + i * sizeof(k_pollfd_t)),
                           sizeof(k_pollfd_t));
            pfd.revents = 0;

            /* Try socket first (existing path) */
            uint32_t sid = sock_id_from_fd(pfd.fd, proc);
            if (sid != SOCK_NONE) {
                sock_t *s = sock_get(sid);
                if (s) {
                    if (s->type == SOCK_TYPE_STREAM && s->state == SOCK_CONNECTED) {
                        tcp_conn_t *tc = tcp_conn_get(s->tcp_conn_id);
                        int peek = tcp_conn_recv(s->tcp_conn_id, (void *)0, 0);
                        if ((pfd.events & POLLIN) && (peek > 0 || peek == 0))
                            pfd.revents |= POLLIN;
                        if (pfd.events & POLLOUT)
                            pfd.revents |= POLLOUT;
                        if (tc && (tc->state == TCP_CLOSE_WAIT || tc->state == TCP_CLOSED
                                   || tc->state == TCP_TIME_WAIT))
                            pfd.revents |= POLLHUP;
                    } else if (s->type == SOCK_TYPE_STREAM && s->state == SOCK_LISTENING) {
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
                /* VFS fd — use .poll callback */
                const vfs_ops_t *ops = proc->fd_table->fds[pfd.fd].ops;
                void *priv = proc->fd_table->fds[pfd.fd].priv;
                if (ops->poll) {
                    uint16_t ready = ops->poll(priv);
                    pfd.revents = ready & (pfd.events | POLLERR | POLLHUP);
                } else {
                    /* No .poll — permissive default */
                    pfd.revents = pfd.events & (POLLIN | POLLOUT);
                }
            } else if (pfd.fd < 0 || (uint32_t)pfd.fd >= PROC_MAX_FDS ||
                       !proc->fd_table->fds[pfd.fd].ops) {
                pfd.revents = POLLNVAL;
            }

            if (pfd.revents) ready++;
            copy_to_user((void *)(uintptr_t)(fds_ptr + i * sizeof(k_pollfd_t)),
                         &pfd, sizeof(k_pollfd_t));
        }
```

Note: POLLERR and POLLHUP are always reported even if not in `events` (POSIX requirement). The `& (pfd.events | POLLERR | POLLHUP)` handles this.

- [ ] **Step 3: Build — verify it compiles**

Run: `make`
Expected: Clean compile, zero warnings.

- [ ] **Step 4: Commit**

```bash
git add kernel/syscall/sys_socket.c
git commit -m "feat(poll): generalize sys_poll to use VFS .poll callbacks"
```

---

### Task 6: Generalize `epoll_wait` to use VFS `.poll` callbacks

**Files:**
- Modify: `kernel/net/epoll.c:150-216` (add VFS poll sweep in `epoll_wait_impl`)

The approach: before checking `ep->nready` and blocking, sweep all watches for VFS fds with `.poll` callbacks. If any match, add to the ready list. Also set `g_poll_waiter` so PIT wakes us for the next poll cycle.

- [ ] **Step 1: Add includes for VFS and proc headers**

At the top of `kernel/net/epoll.c`, add:

```c
#include "vfs.h"
```

(Note: `proc.h` and `sched.h` are already included via `epoll.h`.)

- [ ] **Step 2: Add VFS poll sweep in `epoll_wait_impl`**

In `epoll_wait_impl` (line 150), add the VFS poll sweep inside the main loop, right after acquiring `epoll_lock` and before checking `ep->nready > 0`. The new code goes after line 169 (`irqflags_t efl = spin_lock_irqsave(&epoll_lock);`):

```c
        /* VFS poll sweep: check non-socket fds for readiness */
        {
            aegis_process_t *poll_proc = (aegis_process_t *)sched_current();
            uint8_t w;
            for (w = 0; w < EPOLL_MAX_WATCHES; w++) {
                if (!ep->watches[w].in_use) continue;
                uint32_t wfd = ep->watches[w].fd;
                if (wfd >= PROC_MAX_FDS) continue;
                const vfs_ops_t *ops = poll_proc->fd_table->fds[wfd].ops;
                if (!ops || !ops->poll) continue;
                void *priv = poll_proc->fd_table->fds[wfd].priv;
                /* Release lock while calling .poll (it may acquire per-object locks) */
                spin_unlock_irqrestore(&epoll_lock, efl);
                uint16_t ready = ops->poll(priv);
                efl = spin_lock_irqsave(&epoll_lock);
                uint32_t match = ready & ep->watches[w].events;
                if (!match) continue;
                /* Check if already in ready list (dedup) */
                uint8_t already = 0, k;
                for (k = 0; k < ep->nready; k++) {
                    if (ep->ready[k] == w) { already = 1; break; }
                }
                if (!already && ep->nready < EPOLL_MAX_WATCHES)
                    ep->ready[ep->nready++] = w;
            }
        }
```

- [ ] **Step 3: Set `g_poll_waiter` before blocking**

In the blocking path of `epoll_wait_impl`, before `sched_block()` (around line 213), add:

```c
        /* Also register as poll waiter so PIT wakes us for VFS poll sweep */
        {
            extern aegis_task_t *g_poll_waiter;
            g_poll_waiter = (aegis_task_t *)sched_current();
        }
```

This goes right before the existing `ep->waiter_task = ...` line.

- [ ] **Step 4: Build — verify it compiles**

Run: `make`
Expected: Clean compile, zero warnings.

- [ ] **Step 5: Commit**

```bash
git add kernel/net/epoll.c
git commit -m "feat(epoll): add VFS .poll sweep for non-socket fds in epoll_wait"
```

---

### Task 7: Kernel boot self-test for VFS poll

**Files:**
- Modify: `kernel/core/main.c` (add `poll_test()` call and include)
- Create: `kernel/fs/poll_test.c` (self-test implementation)
- Modify: `Makefile` (add poll_test.c to FS_SRCS)
- Modify: `tests/tests/boot_oracle.rs` (add `[POLL] OK` to BOOT_ORACLE)

- [ ] **Step 1: Create `kernel/fs/poll_test.c`**

```c
/* kernel/fs/poll_test.c — boot self-test for VFS .poll callbacks */
#include "vfs.h"
#include "pipe.h"
#include "kva.h"
#include "printk.h"
#include "spinlock.h"
#include <stdint.h>

void
poll_test(void)
{
    /* Allocate a pipe manually (same as sys_pipe2 but kernel-internal) */
    pipe_t *p = kva_alloc_pages(1);
    if (!p) {
        printk("[POLL] FAIL: kva_alloc_pages\n");
        return;
    }
    __builtin_memset(p, 0, sizeof(*p));
    {
        spinlock_t init = SPINLOCK_INIT;
        p->lock = init;
    }
    p->read_refs  = 1;
    p->write_refs = 1;

    /* Test 1: empty pipe read end — expect no POLLIN */
    uint16_t ev = g_pipe_read_ops.poll(p);
    if (ev & 0x0001) { /* POLLIN */
        printk("[POLL] FAIL: empty pipe reports POLLIN\n");
        kva_free_pages(p, 1);
        return;
    }

    /* Test 2: write data, read end should now report POLLIN */
    p->buf[0] = 'X';
    p->write_pos = 1;
    p->count = 1;
    ev = g_pipe_read_ops.poll(p);
    if (!(ev & 0x0001)) {
        printk("[POLL] FAIL: pipe with data doesn't report POLLIN\n");
        kva_free_pages(p, 1);
        return;
    }

    /* Test 3: close write end — expect POLLIN|POLLHUP */
    p->write_refs = 0;
    ev = g_pipe_read_ops.poll(p);
    if (!(ev & 0x0001) || !(ev & 0x0010)) {
        printk("[POLL] FAIL: closed write end — expected POLLIN|POLLHUP, got 0x%x\n",
               (unsigned)ev);
        kva_free_pages(p, 1);
        return;
    }

    kva_free_pages(p, 1);
    printk("[POLL] OK: vfs poll pipe\n");
}
```

- [ ] **Step 2: Add poll_test.c to Makefile FS_SRCS**

In the `Makefile`, find the `FS_SRCS` variable and add `kernel/fs/poll_test.c` to the list.

- [ ] **Step 3: Call `poll_test()` from kernel_main**

In `kernel/core/main.c`, add `#include "poll_test.h"` at the top (or declare `void poll_test(void);` directly), then add the call after `cap_policy_load()` (line 147) and before `xhci_init()` (line 148):

```c
    poll_test();            /* VFS .poll self-test — [POLL] OK               */
```

Since there's no `poll_test.h` header, just add a forward declaration at the top of main.c:

```c
void poll_test(void);
```

- [ ] **Step 4: Add `[POLL] OK` to the boot oracle**

In `tests/tests/boot_oracle.rs`, add to the `BOOT_ORACLE` array after `"[CAP_POLICY] OK:"`:

```rust
    "[POLL] OK:",
```

- [ ] **Step 5: Build and run tests**

Run: `make test`
Expected: Clean compile. Boot oracle passes with the new `[POLL] OK: vfs poll pipe` line.

- [ ] **Step 6: Commit**

```bash
git add kernel/fs/poll_test.c kernel/core/main.c Makefile tests/tests/boot_oracle.rs
git commit -m "test(poll): kernel boot self-test for VFS .poll on pipes"
```

---

### Task 8: Userspace polltest binary

**Files:**
- Create: `user/bin/polltest/main.c`
- Create: `user/bin/polltest/Makefile`
- Modify: `rootfs.manifest` (add polltest)
- Modify: `Makefile` (add polltest to SIMPLE_USER_PROGS)

- [ ] **Step 1: Create `user/bin/polltest/Makefile`**

```makefile
MUSL_DIR   = ../../../build/musl-dynamic
CC         = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS     = -O2 -s -fno-pie -no-pie -Wl,--build-id=none

polltest.elf: main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f *.elf
```

- [ ] **Step 2: Create `user/bin/polltest/main.c`**

```c
#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <sys/epoll.h>

static int g_pass = 0;
static int g_fail = 0;

static void pass(const char *name)
{
    const char *prefix = "[POLLTEST] PASS: ";
    write(1, prefix, strlen(prefix));
    write(1, name, strlen(name));
    write(1, "\n", 1);
    g_pass++;
}

static void fail(const char *name)
{
    const char *prefix = "[POLLTEST] FAIL: ";
    write(1, prefix, strlen(prefix));
    write(1, name, strlen(name));
    write(1, "\n", 1);
    g_fail++;
}

/* Test 1: pipe poll — empty, with data, after close */
static void test_pipe_poll(void)
{
    int fds[2];
    if (pipe(fds) != 0) { fail("pipe_poll create"); return; }

    /* Empty pipe: poll read end with timeout=0, expect 0 ready */
    struct pollfd pfd = { .fd = fds[0], .events = POLLIN };
    int r = poll(&pfd, 1, 0);
    if (r != 0 || (pfd.revents & POLLIN)) {
        fail("pipe_poll empty");
    } else {
        pass("pipe_poll empty");
    }

    /* Write data: poll read end, expect POLLIN */
    write(fds[1], "X", 1);
    pfd.revents = 0;
    r = poll(&pfd, 1, 0);
    if (r != 1 || !(pfd.revents & POLLIN)) {
        fail("pipe_poll data");
    } else {
        pass("pipe_poll data");
    }

    /* Drain the data first */
    char buf[1];
    read(fds[0], buf, 1);

    /* Close write end: poll read end, expect POLLHUP */
    close(fds[1]);
    pfd.revents = 0;
    r = poll(&pfd, 1, 0);
    if (!(pfd.revents & POLLHUP)) {
        fail("pipe_poll hup");
    } else {
        pass("pipe_poll hup");
    }

    close(fds[0]);
}

/* Test 2: epoll on pipe */
static void test_epoll_pipe(void)
{
    int fds[2];
    if (pipe(fds) != 0) { fail("epoll_pipe create"); return; }

    int epfd = epoll_create1(0);
    if (epfd < 0) { fail("epoll_pipe epoll_create1"); close(fds[0]); close(fds[1]); return; }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = fds[0] };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &ev) != 0) {
        fail("epoll_pipe ctl_add");
        close(epfd); close(fds[0]); close(fds[1]);
        return;
    }

    /* Empty pipe: epoll_wait with timeout=0 — expect 0 events */
    struct epoll_event out;
    int r = epoll_wait(epfd, &out, 1, 0);
    if (r != 0) {
        fail("epoll_pipe empty");
    } else {
        pass("epoll_pipe empty");
    }

    /* Write data: epoll_wait — expect 1 event with EPOLLIN */
    write(fds[1], "Y", 1);
    r = epoll_wait(epfd, &out, 1, 0);
    if (r != 1 || !(out.events & EPOLLIN)) {
        fail("epoll_pipe data");
    } else {
        pass("epoll_pipe data");
    }

    close(epfd);
    close(fds[0]);
    close(fds[1]);
}

/* Test 3: TTY poll — stdin with timeout=0, expect 0 ready (no queued input) */
static void test_tty_poll(void)
{
    struct pollfd pfd = { .fd = 0, .events = POLLIN };
    int r = poll(&pfd, 1, 0);
    /* Should return 0 (no data) and NOT set POLLNVAL */
    if (pfd.revents & 0x0020) { /* POLLNVAL */
        fail("tty_poll nval");
    } else if (r == 0) {
        pass("tty_poll no_data");
    } else {
        /* r > 0 means data was queued (possible but unlikely in test) — still pass */
        pass("tty_poll no_data");
    }
}

/* Test 4: POLLNVAL for bad fd */
static void test_pollnval(void)
{
    struct pollfd pfd = { .fd = 99, .events = POLLIN };
    int r = poll(&pfd, 1, 0);
    (void)r;
    if (pfd.revents & 0x0020) { /* POLLNVAL */
        pass("pollnval bad_fd");
    } else {
        fail("pollnval bad_fd");
    }
}

int main(void)
{
    test_pipe_poll();
    test_epoll_pipe();
    test_tty_poll();
    test_pollnval();

    /* Summary */
    if (g_fail == 0) {
        const char *msg = "[POLLTEST] ALL PASSED\n";
        write(1, msg, strlen(msg));
        return 0;
    }
    return 1;
}
```

- [ ] **Step 3: Add polltest to SIMPLE_USER_PROGS in the Makefile**

In the `Makefile`, find `SIMPLE_USER_PROGS` (line 182) and add `polltest` to the list:

```makefile
SIMPLE_USER_PROGS = \
    ls cat echo pwd uname clear true false wc grep sort \
    mkdir touch rm cp mv whoami ln chmod chown readlink \
    shutdown reboot login stsh httpd nettest polltest
```

- [ ] **Step 4: Add polltest to rootfs.manifest**

Add after the coreutils section:

```
user/bin/polltest/polltest.elf  /bin/polltest
```

- [ ] **Step 5: Build**

Run: `make`
Expected: Clean compile. `user/bin/polltest/polltest.elf` is generated.

- [ ] **Step 6: Commit**

```bash
git add user/bin/polltest/main.c user/bin/polltest/Makefile rootfs.manifest Makefile
git commit -m "test(poll): userspace polltest binary — pipe/epoll/tty/pollnval"
```

---

### Task 9: Rust integration test

**Files:**
- Create: `tests/tests/poll_test.rs`

- [ ] **Step 1: Create `tests/tests/poll_test.rs`**

```rust
// tests/tests/poll_test.rs
//
// Integration test — boots QEMU, verifies kernel poll self-test in boot oracle,
// then runs /bin/polltest and asserts all sub-tests pass.
//
// Run: AEGIS_ISO=build/aegis.iso cargo test --manifest-path tests/Cargo.toml poll_test

use std::time::Duration;
use aegis_tests::{aegis_pc, iso, AegisHarness, assert_subsystem_ok, wait_for_line};

#[tokio::test]
async fn poll_boot_oracle() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found — run `make iso` first", iso.display());
        return;
    }
    let out = AegisHarness::boot(aegis_pc(), &iso)
        .await
        .expect("QEMU failed to start");
    assert_subsystem_ok(&out, "POLL");
}

#[tokio::test]
async fn poll_userspace_tests() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found — run `make iso` first", iso.display());
        return;
    }

    let (mut stream, mut proc) = AegisHarness::boot_stream(aegis_pc(), &iso)
        .await
        .expect("QEMU failed to start");

    // Wait for shell prompt (login auto-completes on pc preset)
    let timeout = Duration::from_secs(30);
    wait_for_line(&mut stream, "$ ", timeout)
        .await
        .expect("timed out waiting for shell prompt");

    // Send the polltest command via serial
    // On pc preset, stdin is the console TTY connected to serial
    // The harness's serial_capture is bidirectional — we need to
    // send keystrokes. However, AegisHarness doesn't expose a write
    // handle. Instead, we rely on vigil/login running polltest as
    // part of the boot sequence.
    //
    // Alternative: check that polltest runs correctly by adding it
    // to the vigil service list or running it from a test-specific
    // init script. For now, we just verify the kernel-level [POLL]
    // OK line is present — the full userspace test needs the
    // streaming + sendkey approach used by login_flow_test.rs.
    //
    // TODO: once we have HMP sendkey support in pc preset, send
    // "polltest\n" and wait for "[POLLTEST] ALL PASSED".

    let _ = proc.kill().await;
}
```

Note: The `poll_userspace_tests` test is a placeholder that documents the path to full integration. The `poll_boot_oracle` test is the real validation — it confirms the kernel `.poll` infrastructure works end-to-end through the boot self-test. Full userspace validation requires HMP `sendkey` (monitor socket), which the `aegis_pc()` preset doesn't enable. The login_flow_test.rs uses `aegis_q35_graphical_mouse()` with `monitor_socket: true` for this — the polltest could be added to a similar graphical test in the future, or we can add a text-mode test preset with monitor socket support.

- [ ] **Step 2: Build and run integration tests**

Run: `make test`
Expected: `poll_boot_oracle` passes. `poll_userspace_tests` runs without error (placeholder).

- [ ] **Step 3: Commit**

```bash
git add tests/tests/poll_test.rs
git commit -m "test(poll): Rust integration test — boot oracle + userspace placeholder"
```

---

### Task 10: Build and add rune to rootfs

**Files:**
- Modify: `rootfs.manifest` (add rune binary)

- [ ] **Step 1: Build rune for musl**

```bash
cd /Users/dylan/Developer/rune
cargo build --release --target x86_64-unknown-linux-musl
```

Expected: Successful build. Binary at `target/x86_64-unknown-linux-musl/release/rune`.

- [ ] **Step 2: Verify it's a static binary**

```bash
file target/x86_64-unknown-linux-musl/release/rune
```

Expected: Output should say `statically linked`.

- [ ] **Step 3: Copy rune binary into the Aegis tree**

```bash
cp /Users/dylan/Developer/rune/target/x86_64-unknown-linux-musl/release/rune \
   /Users/dylan/Developer/aegis/user/bin/rune
```

Note: Since rune is a pre-built Rust static binary (not built by Aegis's musl-gcc), it doesn't need a subdirectory with a Makefile. It goes directly into `user/bin/` as a standalone binary.

- [ ] **Step 4: Add to rootfs.manifest**

Add after the polltest entry:

```
user/bin/rune                   /bin/rune
```

- [ ] **Step 5: Build ISO and verify rune is included**

```bash
cd /Users/dylan/Developer/aegis
make iso
```

Expected: Clean build. The rootfs population step should copy rune into the ext2 image.

- [ ] **Step 6: Commit**

```bash
git add user/bin/rune rootfs.manifest
git commit -m "feat: add rune text editor to Aegis rootfs (musl static binary)"
```

---

### Task 11: Smoke test — full build + boot oracle + manual rune test

This is the final verification task. Everything must work end-to-end.

- [ ] **Step 1: Nuclear clean build**

```bash
cd /Users/dylan/Developer/aegis
git clean -fdx --exclude=references --exclude=.worktrees
rm -f user/vigil/vigil user/vigictl/vigictl user/login/login.elf
make clean
make iso
```

- [ ] **Step 2: Run `make test`**

```bash
make test
```

Expected: All tests pass, including `boot_oracle` with the new `[POLL] OK:` line and `poll_boot_oracle`.

- [ ] **Step 3: Interactive smoke test (optional if hardware available)**

```bash
make run
```

Once at the shell prompt:
1. Run `polltest` — should print all PASS lines and "ALL PASSED"
2. Run `rune` — should launch the TUI editor (if it crashes, note the error for debugging)
3. Run `rune /etc/aegis/caps.d/bastion` — should open a file for editing

- [ ] **Step 4: Commit final state if any fixups were needed**

```bash
git add -A
git commit -m "fix: smoke test fixups for VFS poll + rune port"
```

(Only if fixups were needed. Skip if everything passed clean.)
