# Phase 32: TTY/PTY Layer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a shared TTY abstraction with line discipline, pseudo-terminal support (16 PTY pairs via `/dev/ptmx` + `/dev/pts/N`), proper sessions with controlling terminals, and correct job control.

**Architecture:** Extract line discipline from `kbd_vfs.c` into a shared `tty.c` used by both the console TTY and PTY slaves. PTY pairs are master/slave fd pairs connected by ring buffers. Session leaders acquire controlling terminals; SIGTTIN/SIGTTOU enforce foreground-only terminal access.

**Tech Stack:** C kernel code; musl-gcc user test binary; Python integration test.

**Critical build note:** The Makefile lacks `-MMD` header dependency tracking. After modifying any `.h` file, you MUST use `rm -rf build && make` to ensure all dependent `.c` files are recompiled.

**Remote build:** This project builds and tests on `dylan@10.0.0.19` via SSH key `/Users/dylan/.ssh/aegis/id_ed25519`. Always `rm -rf build` before `make` when headers change.

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `kernel/fs/tty.h` | Create | `tty_t` struct, `k_termios_t`, termios constants, tty_read/write/ioctl/init_defaults |
| `kernel/fs/tty.c` | Create | Shared line discipline: canonical/raw read, echo, signal generation, SIGTTIN/SIGTTOU |
| `kernel/fs/pty.h` | Create | `pty_pair_t`, ptmx_open, pts_open, pty_find_by_session |
| `kernel/fs/pty.c` | Create | PTY pair pool, master/slave VFS ops, ring buffers |
| `kernel/fs/kbd_vfs.c` | Modify | Replace inline line discipline with delegation to console `tty_t` via tty_read |
| `kernel/fs/console.c` | Modify | Expose console_write_fn as tty write_out callback |
| `kernel/fs/vfs.c` | Modify | Add `/dev/ptmx` and `/dev/pts/` dispatch |
| `kernel/arch/x86_64/kbd.c` | Modify | Remove signal generation; push raw chars only |
| `kernel/proc/proc.h` | Modify | Add `sid` field |
| `kernel/proc/proc.c` | Modify | Initialize `sid` in proc_spawn |
| `kernel/syscall/sys_process.c` | Modify | Fix setsid, relax setpgid, SIGHUP on session leader exit |
| `kernel/syscall/sys_file.c` | Modify | Route ioctl for PTY/tty fds through tty_ioctl |
| `kernel/signal/signal.c` | Modify | Session leader exit → SIGHUP |
| `Makefile` | Modify | Add tty.c and pty.c to FS_SRCS, proc_test to DISK_USER_BINS |
| `user/pty_test/main.c` | Create | PTY test binary |
| `user/pty_test/Makefile` | Create | musl-gcc static build |
| `tests/test_pty.py` | Create | Integration test |
| `tests/run_tests.sh` | Modify | Add test_pty.py |

---

### Task 1: Create tty.h — TTY Type Definitions and Shared Interface

The first task creates the header that both the console and PTY backends will use. No implementation yet — just types and declarations.

**Files:**
- Create: `kernel/fs/tty.h`

- [ ] **Step 1: Create tty.h**

```c
/* kernel/fs/tty.h — shared TTY abstraction */
#ifndef AEGIS_TTY_H
#define AEGIS_TTY_H

#include <stdint.h>

/* ── termios (must match musl x86_64 struct termios exactly: 60 bytes) ── */

#define K_NCCS   32

/* c_lflag bits */
#define K_ISIG   0x01U   /* enable signal generation (INTR/QUIT/SUSP) */
#define K_ICANON 0x02U   /* canonical (line-buffered) mode */
#define K_ECHO   0x08U   /* echo input characters */
#define K_ECHOE  0x10U   /* echo erase as BS-SP-BS */
#define K_ECHOK  0x20U   /* echo NL after kill */
#define K_TOSTOP 0x100U  /* send SIGTTOU for background output */

/* c_iflag bits */
#define K_ICRNL  0x100U  /* translate CR to NL on input */
#define K_OPOST  0x01U   /* enable output processing */
#define K_ONLCR  0x04U   /* map NL to CR-NL on output */

/* c_cc indices */
#define K_VINTR   0
#define K_VQUIT   1
#define K_VERASE  2
#define K_VKILL   3
#define K_VEOF    4
#define K_VTIME   5
#define K_VMIN    6
#define K_VSUSP   10

typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[K_NCCS];
    /* 3 bytes natural padding between c_cc[32] (offset 49) and c_ispeed (offset 52) */
    uint32_t c_ispeed;
    uint32_t c_ospeed;
} k_termios_t;

_Static_assert(sizeof(k_termios_t) == 60,
    "k_termios_t must be 60 bytes to match musl struct termios");

/* ── tty_t — shared terminal abstraction ──────────────────────────────── */

#define TTY_LINEBUF_MAX 512

typedef struct tty {
    k_termios_t termios;
    uint32_t    fg_pgrp;       /* foreground process group */
    uint32_t    session_id;    /* controlling session (0 = none) */
    uint16_t    rows, cols;    /* window size */
    char        linebuf[TTY_LINEBUF_MAX]; /* canonical mode buffer */
    uint32_t    line_len;      /* bytes in linebuf */
    uint32_t    line_pos;      /* next byte to return to caller */
    uint8_t     line_ready;    /* line complete (Enter pressed) */

    /* Backend callbacks — different for console vs PTY */
    int  (*write_out)(struct tty *tty, const char *buf, uint32_t len);
    int  (*read_raw)(struct tty *tty, char *out, int *interrupted);
    void *ctx;                 /* backend-private pointer */
} tty_t;

/* tty_init_defaults — set termios to sane defaults (canonical, echo, ISIG).
 * Does NOT set backend callbacks — caller must set write_out/read_raw/ctx. */
void tty_init_defaults(tty_t *tty);

/* tty_read — line-discipline read. Handles canonical/raw mode, echo,
 * backspace, Ctrl-D (EOF), signal generation (Ctrl-C/Z/\).
 * buf is a KERNEL buffer. Returns bytes read, 0 for EOF, -4 for EINTR,
 * -5 for EIO (hangup). */
int tty_read(tty_t *tty, char *buf, uint64_t len);

/* tty_write — output processing. If OPOST+ONLCR, maps NL → CR+NL.
 * buf is a KERNEL buffer. Returns bytes consumed from buf. */
int tty_write(tty_t *tty, const char *buf, uint64_t len);

/* tty_ioctl — handle termios/winsize/pgrp ioctls.
 * arg is a USER pointer. Returns 0 or negative errno. */
int tty_ioctl(tty_t *tty, uint64_t cmd, uint64_t arg);

/* tty_is_fg — returns 1 if the current process is in the foreground
 * process group for this tty. Used for SIGTTIN/SIGTTOU checks. */
int tty_is_fg(tty_t *tty);

/* Console TTY singleton — initialized by kbd_vfs.c at first use. */
tty_t *tty_console(void);

/* Find controlling tty for current process's session.
 * Scans console + PTY pool. Returns NULL if none. */
tty_t *tty_find_controlling(uint32_t session_id);

#endif /* AEGIS_TTY_H */
```

- [ ] **Step 2: Commit**

```bash
git add kernel/fs/tty.h
git commit -m "feat: add tty.h — shared TTY type definitions and interface"
```

---

### Task 2: Create tty.c — Shared Line Discipline

Extract line discipline logic from `kbd_vfs.c` into `tty.c`. This is the core refactoring — the canonical/raw read, echo, backspace, Ctrl-D, signal generation all move here.

**Files:**
- Create: `kernel/fs/tty.c`
- Modify: `Makefile` (add tty.c to FS_SRCS)

- [ ] **Step 1: Create tty.c**

```c
/* kernel/fs/tty.c — shared TTY line discipline */
#include "tty.h"
#include "printk.h"
#include "uaccess.h"
#include "signal.h"
#include "proc.h"
#include "sched.h"
#include <stdint.h>

/* ── init ─────────────────────────────────────────────────────────────── */

void
tty_init_defaults(tty_t *tty)
{
    __builtin_memset(tty, 0, sizeof(*tty));
    tty->termios.c_iflag       = K_ICRNL;
    tty->termios.c_oflag       = K_OPOST | K_ONLCR;
    tty->termios.c_lflag       = K_ICANON | K_ECHO | K_ISIG;
    tty->termios.c_cc[K_VINTR] = 0x03; /* Ctrl-C */
    tty->termios.c_cc[K_VQUIT] = 0x1C; /* Ctrl-\ */
    tty->termios.c_cc[K_VERASE]= 0x7F; /* DEL */
    tty->termios.c_cc[K_VEOF]  = 0x04; /* Ctrl-D */
    tty->termios.c_cc[K_VSUSP] = 0x1A; /* Ctrl-Z */
    tty->termios.c_cc[K_VMIN]  = 1;
    tty->termios.c_cc[K_VTIME] = 0;
    tty->rows = 25;
    tty->cols = 80;
}

/* ── internal helpers ─────────────────────────────────────────────────── */

static void
tty_echo_char(tty_t *tty, char c)
{
    if (!(tty->termios.c_lflag & K_ECHO))
        return;
    tty->write_out(tty, &c, 1);
}

static void
tty_echo_str(tty_t *tty, const char *s, uint32_t len)
{
    if (!(tty->termios.c_lflag & K_ECHO))
        return;
    tty->write_out(tty, s, len);
}

/* ── foreground check ─────────────────────────────────────────────────── */

int
tty_is_fg(tty_t *tty)
{
    if (tty->fg_pgrp == 0)
        return 1; /* no foreground group set — allow */
    aegis_task_t *cur = sched_current();
    if (!cur || !cur->is_user)
        return 1;
    aegis_process_t *proc = (aegis_process_t *)cur;
    return proc->pgid == tty->fg_pgrp;
}

/* ── tty_read — line discipline read ──────────────────────────────────── */

int
tty_read(tty_t *tty, char *buf, uint64_t len)
{
    if (len == 0) return 0;

    /* SIGTTIN: background process reading from controlling tty */
    if (!tty_is_fg(tty)) {
        aegis_process_t *proc = (aegis_process_t *)sched_current();
        signal_send_pgrp(proc->pgid, 20 /* SIGTTIN */);
        return -4; /* EINTR */
    }

    int raw = !(tty->termios.c_lflag & K_ICANON);

    /* RAW mode: one character immediately */
    if (raw) {
        int interrupted;
        char c;
        if (!tty->read_raw)
            return -5; /* EIO */
        c = tty->read_raw(tty, &c, &interrupted);
        if (interrupted) return -4; /* EINTR */
        buf[0] = c;
        return 1;
    }

    /* COOKED mode: return buffered bytes from a previously-read line */
    if (tty->line_pos < tty->line_len) {
        buf[0] = tty->linebuf[tty->line_pos++];
        if (tty->line_pos >= tty->line_len) {
            tty->line_len = 0;
            tty->line_pos = 0;
        }
        return 1;
    }

    /* COOKED mode: read and echo characters until Enter / EOF */
    tty->line_len = 0;
    tty->line_pos = 0;

    for (;;) {
        int interrupted;
        char c;
        if (!tty->read_raw)
            return -5; /* EIO */
        tty->read_raw(tty, &c, &interrupted);
        if (interrupted) {
            tty->line_len = 0;
            tty->line_pos = 0;
            return -4; /* EINTR */
        }

        /* CR→NL translation */
        if ((tty->termios.c_iflag & K_ICRNL) && c == '\r')
            c = '\n';

        /* Signal generation (ISIG) */
        if (tty->termios.c_lflag & K_ISIG) {
            if ((uint8_t)c == tty->termios.c_cc[K_VINTR] && tty->fg_pgrp) {
                signal_send_pgrp(tty->fg_pgrp, 2 /* SIGINT */);
                tty->line_len = 0;
                tty->line_pos = 0;
                tty_echo_str(tty, "^C\n", 3);
                return -4; /* EINTR */
            }
            if ((uint8_t)c == tty->termios.c_cc[K_VSUSP] && tty->fg_pgrp) {
                signal_send_pgrp(tty->fg_pgrp, 20 /* SIGTSTP */);
                tty->line_len = 0;
                tty->line_pos = 0;
                tty_echo_str(tty, "^Z\n", 3);
                return -4; /* EINTR */
            }
            if ((uint8_t)c == tty->termios.c_cc[K_VQUIT] && tty->fg_pgrp) {
                signal_send_pgrp(tty->fg_pgrp, 3 /* SIGQUIT */);
                tty->line_len = 0;
                tty->line_pos = 0;
                return -4; /* EINTR */
            }
        }

        /* Newline / Enter — flush line */
        if (c == '\n') {
            if (tty->line_len < TTY_LINEBUF_MAX - 1)
                tty->linebuf[tty->line_len++] = '\n';
            tty_echo_char(tty, '\n');
            break;
        }

        /* EOF (Ctrl-D) */
        if ((uint8_t)c == tty->termios.c_cc[K_VEOF]) {
            if (tty->line_len == 0)
                return 0; /* EOF */
            break; /* flush pending input without newline */
        }

        /* Erase (backspace/DEL) */
        if ((uint8_t)c == tty->termios.c_cc[K_VERASE] || c == '\b') {
            if (tty->line_len > 0) {
                tty->line_len--;
                tty_echo_str(tty, "\b \b", 3);
            }
            continue;
        }

        /* Printable character */
        if ((uint8_t)c >= 0x20 && tty->line_len < TTY_LINEBUF_MAX - 1) {
            tty->linebuf[tty->line_len++] = c;
            tty_echo_char(tty, c);
        }
    }

    /* Return first buffered byte */
    buf[0] = tty->linebuf[tty->line_pos++];
    if (tty->line_pos >= tty->line_len) {
        tty->line_len = 0;
        tty->line_pos = 0;
    }
    return 1;
}

/* ── tty_write — output processing ────────────────────────────────────── */

int
tty_write(tty_t *tty, const char *buf, uint64_t len)
{
    if (!tty->write_out)
        return -5; /* EIO */

    /* SIGTTOU: background process writing to tty with TOSTOP */
    if ((tty->termios.c_lflag & K_TOSTOP) && !tty_is_fg(tty)) {
        aegis_process_t *proc = (aegis_process_t *)sched_current();
        signal_send_pgrp(proc->pgid, 22 /* SIGTTOU */);
        return -4; /* EINTR */
    }

    /* Output processing: NL→CRNL if OPOST+ONLCR */
    if ((tty->termios.c_oflag & K_OPOST) &&
        (tty->termios.c_oflag & K_ONLCR)) {
        uint64_t i;
        for (i = 0; i < len; i++) {
            if (buf[i] == '\n') {
                tty->write_out(tty, "\r\n", 2);
            } else {
                tty->write_out(tty, &buf[i], 1);
            }
        }
        return (int)len;
    }

    return tty->write_out(tty, buf, (uint32_t)len);
}

/* ── tty_ioctl ────────────────────────────────────────────────────────── */

#define TCGETS    0x5401UL
#define TCSETS    0x5402UL
#define TCSETSW   0x5403UL
#define TCSETSF   0x5404UL
#define TIOCGPGRP 0x540FUL
#define TIOCSPGRP 0x5410UL
#define TIOCGWINSZ 0x5413UL
#define TIOCSWINSZ 0x5414UL

int
tty_ioctl(tty_t *tty, uint64_t cmd, uint64_t arg)
{
    switch (cmd) {
    case TCGETS: {
        if (!user_ptr_valid(arg, sizeof(k_termios_t)))
            return -14; /* EFAULT */
        copy_to_user((void *)(uintptr_t)arg, &tty->termios, sizeof(k_termios_t));
        return 0;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        if (!user_ptr_valid(arg, sizeof(k_termios_t)))
            return -14; /* EFAULT */
        copy_from_user(&tty->termios, (const void *)(uintptr_t)arg, sizeof(k_termios_t));
        return 0;
    }
    case TIOCGPGRP: {
        if (!user_ptr_valid(arg, sizeof(uint32_t)))
            return -14;
        copy_to_user((void *)(uintptr_t)arg, &tty->fg_pgrp, sizeof(uint32_t));
        return 0;
    }
    case TIOCSPGRP: {
        if (!user_ptr_valid(arg, sizeof(uint32_t)))
            return -14;
        uint32_t pgid;
        copy_from_user(&pgid, (const void *)(uintptr_t)arg, sizeof(uint32_t));
        tty->fg_pgrp = pgid;
        return 0;
    }
    case TIOCGWINSZ: {
        uint16_t ws[4];
        ws[0] = tty->rows;
        ws[1] = tty->cols;
        ws[2] = 0;
        ws[3] = 0;
        if (!user_ptr_valid(arg, sizeof(ws)))
            return -14;
        copy_to_user((void *)(uintptr_t)arg, ws, sizeof(ws));
        return 0;
    }
    case TIOCSWINSZ: {
        uint16_t ws[4];
        if (!user_ptr_valid(arg, sizeof(ws)))
            return -14;
        copy_from_user(ws, (const void *)(uintptr_t)arg, sizeof(ws));
        tty->rows = ws[0];
        tty->cols = ws[1];
        /* SIGWINCH to foreground process group */
        if (tty->fg_pgrp)
            signal_send_pgrp(tty->fg_pgrp, 28 /* SIGWINCH */);
        return 0;
    }
    default:
        return -25; /* ENOTTY */
    }
}
```

- [ ] **Step 2: Add tty.c to Makefile FS_SRCS**

In `Makefile`, after `kernel/fs/ramfs.c \`, add (keeping existing `kernel/fs/procfs.c`):
```
    kernel/fs/tty.c \
```

So FS_SRCS ends with:
```
    kernel/fs/ramfs.c \
    kernel/fs/procfs.c \
    kernel/fs/tty.c
```

Wait — check whether procfs.c already has a backslash or not. If procfs.c is the last entry (no backslash), add backslash to it and put `kernel/fs/tty.c` after. The final line should have no backslash.

- [ ] **Step 3: Verify compilation**

The file should compile standalone (no consumers yet). Run: `rm -rf build && make`

- [ ] **Step 4: Commit**

```bash
git add kernel/fs/tty.h kernel/fs/tty.c Makefile
git commit -m "feat: add shared TTY line discipline (tty.h + tty.c)"
```

---

### Task 3: Refactor kbd_vfs.c to Use tty_t Console Backend

This is the critical behavior-preserving refactor. Replace the inline line discipline in `kbd_vfs.c` with delegation to a console `tty_t`. After this task, `make test` must still pass.

**Files:**
- Modify: `kernel/fs/kbd_vfs.c`
- Modify: `kernel/fs/console.c`
- Modify: `kernel/arch/x86_64/kbd.c`

- [ ] **Step 1: Refactor kbd_vfs.c**

The current kbd_vfs.c has ~210 lines including inline k_termios_t definition, line discipline, and tcgets/tcsets. Replace with a thin wrapper around the console tty_t.

Key changes:
1. Remove the inline `k_termios_t` definition and `K_*` constants (now in tty.h)
2. Remove `s_termios`, `s_raw`, `s_linebuf*` (now inside `tty_t`)
3. Remove `kbd_vfs_termios_init` (replaced by `tty_init_defaults`)
4. Replace `kbd_vfs_read_fn` with delegation to `tty_read`
5. Replace `kbd_vfs_tcgets/tcsets` with delegation to `tty_ioctl`
6. Add console tty singleton with `write_out` → printk, `read_raw` → kbd_read_interruptible
7. `kbd_vfs_is_tty` still works (checks ops pointer)

New kbd_vfs.c (complete rewrite — ~100 lines):

```c
#include "kbd_vfs.h"
#include "tty.h"
#include "vfs.h"
#include "kbd.h"
#include "printk.h"
#include "uaccess.h"
#include <stdint.h>

/* ── console tty backend ──────────────────────────────────────────────── */

static tty_t s_console_tty;

static int
console_tty_write_out(tty_t *tty, const char *buf, uint32_t len)
{
    (void)tty;
    uint32_t i;
    for (i = 0; i < len; i++)
        printk("%c", buf[i]);
    return (int)len;
}

static int
console_tty_read_raw(tty_t *tty, char *out, int *interrupted)
{
    (void)tty;
    char c = kbd_read_interruptible(interrupted);
    *out = c;
    return (*interrupted) ? 0 : 1;
}

static void
console_tty_init(void)
{
    tty_init_defaults(&s_console_tty);
    s_console_tty.write_out = console_tty_write_out;
    s_console_tty.read_raw  = console_tty_read_raw;
    s_console_tty.ctx       = (void *)0;
}

tty_t *
tty_console(void)
{
    return &s_console_tty;
}

/* ── VFS ops ──────────────────────────────────────────────────────────── */

static int
kbd_vfs_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
    (void)priv; (void)off;
    return tty_read(&s_console_tty, (char *)buf, len);
}

static int
kbd_vfs_write_fn(void *priv, const void *buf, uint64_t len)
{
    (void)priv; (void)buf; (void)len;
    return -38; /* ENOSYS — stdin is not writable */
}

static void
kbd_vfs_close_fn(void *priv)
{
    (void)priv;
}

static int
kbd_stat_fn(void *priv, k_stat_t *st)
{
    (void)priv;
    __builtin_memset(st, 0, sizeof(*st));
    st->st_mode  = S_IFCHR | 0400;
    st->st_ino   = 3;
    st->st_rdev  = makedev(4, 0);
    st->st_dev   = 1;
    st->st_nlink = 1;
    return 0;
}

static const vfs_ops_t s_kbd_ops = {
    .read    = kbd_vfs_read_fn,
    .write   = kbd_vfs_write_fn,
    .close   = kbd_vfs_close_fn,
    .readdir = (void *)0,
    .dup     = (void *)0,
    .stat    = kbd_stat_fn,
};

static vfs_file_t s_kbd_file = {
    .ops    = &s_kbd_ops,
    .priv   = (void *)0,
    .offset = 0,
    .size   = 0,
};

/* kbd_vfs_is_tty — returns 1 if this fd uses the console kbd ops.
 * Used by sys_ioctl to detect tty fds. TODO: extend for PTY slave fds. */
int
kbd_vfs_is_tty(const vfs_file_t *f)
{
    return f->ops == &s_kbd_ops;
}

/* kbd_vfs_tcgets/tcsets — delegate to tty_ioctl on the console tty. */
int
kbd_vfs_tcgets(void *dst_user)
{
    return tty_ioctl(&s_console_tty, 0x5401UL, (uint64_t)(uintptr_t)dst_user);
}

int
kbd_vfs_tcsets(const void *src_user)
{
    return tty_ioctl(&s_console_tty, 0x5402UL, (uint64_t)(uintptr_t)src_user);
}

vfs_file_t *
kbd_vfs_open(void)
{
    static int s_inited = 0;
    if (!s_inited) {
        console_tty_init();
        s_inited = 1;
    }
    return &s_kbd_file;
}
```

- [ ] **Step 2: Update kbd.c — remove signal generation from keyboard handler**

Signal generation now happens in `tty_read` (tty.c) instead of `kbd_handler`. Remove the Ctrl-C/Z/\ signal delivery from kbd.c — these characters should just be pushed into the ring buffer as raw ASCII.

In `kbd_handler()`, replace the Ctrl-C/Z/\ blocks:
```c
    /* Ctrl-C = Ctrl held + scancode 0x2E ('c') */
    if (s_ctrl && sc == 0x2E) {
        buf_push(0x03); /* ETX — processed by line discipline */
        return;
    }
    if (s_ctrl && sc == 0x2C) {
        buf_push(0x1A); /* SUB — processed by line discipline */
        return;
    }
    if (s_ctrl && sc == 0x2B) {
        buf_push(0x1C); /* FS — processed by line discipline */
        return;
    }
```

In `kbd_usb_inject()`, similarly replace the signal interception with raw pushes:
```c
    /* These control characters are now handled by the TTY line discipline
     * in tty.c, not here. Just push them into the ring buffer. */
    buf_push((char)ascii);
```
(Remove the if/else chain for 0x03/0x1A/0x1C that calls signal_send_pid.)

Remove the `#include "signal.h"`, `#include "proc.h"`, and `#include "sched.h"` from kbd.c if they're only used for signal delivery. Keep kbd_set_tty_pgrp and kbd_get_tty_pgrp for backward compat (they're called from sys_ioctl and sys_setfg) — but they now set s_tty_pgrp which is only used locally (nowhere else reads it directly anymore). Actually, keep them — the console tty's fg_pgrp is set via `tty_ioctl(TIOCSPGRP)` which goes through tty.c. The `kbd_set_tty_pgrp` function should now update the console tty's fg_pgrp instead:

Wait — this gets tricky. `kbd_set_tty_pgrp` is called from proc.c (boot-time) and sys_setfg (custom syscall 360). These need to set the console tty's fg_pgrp. The simplest approach: `kbd_set_tty_pgrp` calls `tty_console()->fg_pgrp = pgid`. But tty_console() is defined in kbd_vfs.c. Circular dependency.

Better approach: keep `s_tty_pgrp` in kbd.c for now. In `console_tty_init()`, the console tty's fg_pgrp is initialized from `kbd_get_tty_pgrp()`. And `kbd_set_tty_pgrp` updates BOTH `s_tty_pgrp` (for backward compat) AND the console tty's fg_pgrp via `tty_console()->fg_pgrp = pgid`.

Actually simplest: remove `s_tty_pgrp` from kbd.c. Change `kbd_set_tty_pgrp` and `kbd_get_tty_pgrp` to operate on `tty_console()->fg_pgrp` directly. kbd.c includes tty.h and calls tty_console(). No circular dependency since tty_console() is declared in tty.h and defined in kbd_vfs.c.

Update kbd.c:
```c
#include "tty.h"

void
kbd_set_tty_pgrp(uint32_t pgid)
{
    tty_console()->fg_pgrp = pgid;
}

uint32_t
kbd_get_tty_pgrp(void)
{
    return tty_console()->fg_pgrp;
}
```

Remove the `static volatile uint32_t s_tty_pgrp = 0;` line and the `#include "signal.h"` / `#include "proc.h"` / `#include "sched.h"` includes (no longer needed).

- [ ] **Step 3: Fix tty_read read_raw callback signature**

Looking at the current code: `kbd_read_interruptible(int *interrupted)` returns `char`. But the `read_raw` callback in tty.h takes `(tty_t *tty, char *out, int *interrupted)` and returns int. The console_tty_read_raw wrapper handles this. Also need to fix tty_read in tty.c — the raw mode path has a bug:

```c
    if (raw) {
        int interrupted;
        char c;
        if (!tty->read_raw)
            return -5;
        tty->read_raw(tty, &c, &interrupted);
        if (interrupted) return -4;
        buf[0] = c;
        return 1;
    }
```

And in the cooked loop:
```c
        tty->read_raw(tty, &c, &interrupted);
```

Make sure the return value of read_raw is used correctly. The console backend wrapper returns 0 on interrupt, 1 on success.

- [ ] **Step 4: Run make test to verify console behavior is preserved**

Run: `rm -rf build && make test`
Expected: PASS — exact same boot.txt output, console works identically.

This is the critical regression gate. If this fails, the refactor has a bug.

- [ ] **Step 5: Commit**

```bash
git add kernel/fs/kbd_vfs.c kernel/arch/x86_64/kbd.c
git commit -m "refactor: move line discipline from kbd_vfs to shared tty.c"
```

---

### Task 4: Add Session ID to Process Struct + Fix setsid/setpgid

**Files:**
- Modify: `kernel/proc/proc.h` (add sid)
- Modify: `kernel/proc/proc.c` (initialize sid)
- Modify: `kernel/syscall/sys_process.c` (fix setsid, relax setpgid)

- [ ] **Step 1: Add sid field to proc.h**

In `aegis_process_t`, after `pgid`:
```c
    uint32_t      sid;            /* session ID; == pid for session leaders */
```

- [ ] **Step 2: Initialize sid in proc.c**

In `proc_spawn()`, after `proc->pgid = proc->pid;`:
```c
    proc->sid = proc->pid;  /* init is its own session leader */
```

In `sys_fork` and `sys_clone` in sys_process.c, after `child->pgid = parent->pgid;`, add:
```c
    child->sid = parent->sid;
```

- [ ] **Step 3: Fix setsid**

Replace the current `sys_setsid` with:
```c
uint64_t
sys_setsid(void)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    /* Cannot create session if already a process group leader
     * (POSIX: prevents session leader from calling setsid again) */
    if (proc->pgid == proc->pid)
        return (uint64_t)-(int64_t)1; /* EPERM */
    proc->sid  = proc->pid;
    proc->pgid = proc->pid;
    /* Detach from controlling terminal — new session has none */
    return (uint64_t)proc->pid;
}
```

- [ ] **Step 4: Relax setpgid**

Replace the `sys_setpgid` to allow joining existing groups in the same session:

```c
uint64_t
sys_setpgid(uint64_t pid_arg, uint64_t pgid_arg)
{
    aegis_process_t *caller = (aegis_process_t *)sched_current();
    uint32_t pid  = (uint32_t)pid_arg;
    uint32_t pgid = (uint32_t)pgid_arg;

    aegis_process_t *target = caller;
    if (pid != 0 && pid != caller->pid) {
        target = proc_find_by_pid(pid);
        if (!target)
            return (uint64_t)-(int64_t)3; /* ESRCH */
        /* Can only setpgid on self or direct child */
        if (target->ppid != caller->pid)
            return (uint64_t)-(int64_t)1; /* EPERM */
    }

    if (pgid == 0)
        pgid = target->pid;

    /* Allow: pgid == target->pid (create own group)
     * Allow: pgid matches an existing group in the same session */
    if (pgid != target->pid) {
        /* Verify pgid belongs to a process in the same session */
        int found = 0;
        aegis_task_t *t = sched_current();
        aegis_task_t *start = t;
        do {
            if (t->is_user) {
                aegis_process_t *p = (aegis_process_t *)t;
                if (p->pgid == pgid && p->sid == target->sid) {
                    found = 1;
                    break;
                }
            }
            t = t->next;
        } while (t != start);
        if (!found)
            return (uint64_t)-(int64_t)1; /* EPERM */
    }

    target->pgid = pgid;
    return 0;
}
```

- [ ] **Step 5: Build and test**

Run: `rm -rf build && make test`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add kernel/proc/proc.h kernel/proc/proc.c kernel/syscall/sys_process.c
git commit -m "feat: add session ID, fix setsid, relax setpgid"
```

---

### Task 5: Create PTY Infrastructure (pty.h + pty.c)

**Files:**
- Create: `kernel/fs/pty.h`
- Create: `kernel/fs/pty.c`
- Modify: `Makefile` (add pty.c to FS_SRCS)

- [ ] **Step 1: Create pty.h**

```c
/* kernel/fs/pty.h — pseudo-terminal pair management */
#ifndef AEGIS_PTY_H
#define AEGIS_PTY_H

#include "vfs.h"
#include "tty.h"
#include <stdint.h>

#define PTY_MAX_PAIRS  16
#define PTY_BUF_SIZE   4096

typedef struct {
    uint8_t  input_buf[PTY_BUF_SIZE];    /* master→slave ring buffer */
    uint32_t input_head, input_tail;
    uint8_t  output_buf[PTY_BUF_SIZE];   /* slave→master ring buffer */
    uint32_t output_head, output_tail;
    tty_t    tty;                         /* slave's TTY */
    uint8_t  master_open;
    uint8_t  slave_open;
    uint8_t  locked;                      /* cleared by unlockpt */
    uint8_t  in_use;
    uint8_t  index;                       /* 0-15 */
} pty_pair_t;

/* ptmx_open — allocate a PTY pair and return the master fd.
 * Returns 0 on success, -12 (ENOMEM) if pool exhausted. */
int ptmx_open(int flags, vfs_file_t *out);

/* pts_open — open the slave side of PTY pair N.
 * Returns 0 on success, -2 (ENOENT) if not allocated,
 * -13 (EACCES) if locked. */
int pts_open(uint32_t index, int flags, vfs_file_t *out);

/* pty_find_by_session — find a PTY whose tty.session_id matches.
 * Returns pointer to tty_t, or NULL. */
tty_t *pty_find_by_session(uint32_t session_id);

/* pty_is_master — returns 1 if the vfs_file is a PTY master fd. */
int pty_is_master(const vfs_file_t *f);

/* pty_is_slave — returns 1 if the vfs_file is a PTY slave fd. */
int pty_is_slave(const vfs_file_t *f);

/* pty_get_tty — if the fd is a PTY slave, return its tty_t. Else NULL. */
tty_t *pty_get_tty(const vfs_file_t *f);

#endif /* AEGIS_PTY_H */
```

- [ ] **Step 2: Create pty.c**

```c
/* kernel/fs/pty.c — pseudo-terminal pair management */
#include "pty.h"
#include "tty.h"
#include "vfs.h"
#include "uaccess.h"
#include "sched.h"
#include "proc.h"
#include "signal.h"
#include <stdint.h>

static pty_pair_t s_pty_pool[PTY_MAX_PAIRS];

/* ── ring buffer helpers ──────────────────────────────────────────────── */

static uint32_t
ring_count(uint32_t head, uint32_t tail, uint32_t size)
{
    return (head - tail) & (size - 1);
}

static uint32_t
ring_space(uint32_t head, uint32_t tail, uint32_t size)
{
    return (size - 1) - ring_count(head, tail, size);
}

static int
ring_push(uint8_t *buf, uint32_t *head, uint32_t tail, uint32_t size,
          const char *data, uint32_t len)
{
    uint32_t avail = ring_space(*head, tail, size);
    if (len > avail) len = avail;
    uint32_t i;
    for (i = 0; i < len; i++) {
        buf[*head] = (uint8_t)data[i];
        *head = (*head + 1) & (size - 1);
    }
    return (int)len;
}

static int
ring_pull(uint8_t *buf, uint32_t head, uint32_t *tail, uint32_t size,
          char *out, uint32_t len)
{
    uint32_t avail = ring_count(head, *tail, size);
    if (len > avail) len = avail;
    uint32_t i;
    for (i = 0; i < len; i++) {
        out[i] = (char)buf[*tail];
        *tail = (*tail + 1) & (size - 1);
    }
    return (int)len;
}

/* ── PTY slave tty_t callbacks ────────────────────────────────────────── */

static int
pty_slave_write_out(tty_t *tty, const char *buf, uint32_t len)
{
    pty_pair_t *pair = (pty_pair_t *)tty->ctx;
    if (!pair->master_open)
        return -5; /* EIO — master hung up */
    int n = ring_push(pair->output_buf, &pair->output_head,
                      pair->output_tail, PTY_BUF_SIZE, buf, len);
    /* Wake master if blocked on read */
    /* (single-core: no explicit wake needed — master polls or blocks) */
    return n;
}

static int
pty_slave_read_raw(tty_t *tty, char *out, int *interrupted)
{
    pty_pair_t *pair = (pty_pair_t *)tty->ctx;
    *interrupted = 0;

    /* Block until input available or master closes */
    __asm__ volatile("sti");
    for (;;) {
        if (ring_count(pair->input_head, pair->input_tail, PTY_BUF_SIZE) > 0) {
            char c;
            ring_pull(pair->input_buf, pair->input_head,
                      &pair->input_tail, PTY_BUF_SIZE, &c, 1);
            __asm__ volatile("cli");
            *out = c;
            return 1;
        }
        if (!pair->master_open) {
            __asm__ volatile("cli");
            return -5; /* EIO — master hung up */
        }
        if (signal_check_pending()) {
            __asm__ volatile("cli");
            *interrupted = 1;
            return 0;
        }
        __asm__ volatile("hlt");
    }
}

/* ── master VFS ops ───────────────────────────────────────────────────── */

static int
master_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
    (void)off;
    pty_pair_t *pair = (pty_pair_t *)priv;

    if (!pair->slave_open &&
        ring_count(pair->output_head, pair->output_tail, PTY_BUF_SIZE) == 0)
        return 0; /* EOF — slave closed and buffer drained */

    /* Block until output available */
    __asm__ volatile("sti");
    for (;;) {
        uint32_t avail = ring_count(pair->output_head, pair->output_tail, PTY_BUF_SIZE);
        if (avail > 0) {
            uint32_t n = (uint32_t)len;
            if (n > avail) n = avail;
            ring_pull(pair->output_buf, pair->output_head,
                      &pair->output_tail, PTY_BUF_SIZE, (char *)buf, n);
            __asm__ volatile("cli");
            return (int)n;
        }
        if (!pair->slave_open) {
            __asm__ volatile("cli");
            return 0; /* EOF */
        }
        if (signal_check_pending()) {
            __asm__ volatile("cli");
            return -4; /* EINTR */
        }
        __asm__ volatile("hlt");
    }
}

static int
master_write_fn(void *priv, const void *buf, uint64_t len)
{
    pty_pair_t *pair = (pty_pair_t *)priv;

    if (!pair->slave_open)
        return -5; /* EIO */

    /* Push data into the input ring buffer for the slave's line discipline */
    static char kbuf[256];
    uint64_t done = 0;
    while (done < len) {
        uint64_t chunk = len - done;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        /* buf is a user pointer from sys_write — copy to kernel first */
        {
            uint64_t page_off = (uint64_t)(uintptr_t)((const uint8_t *)buf + done) & 0xFFFULL;
            uint64_t to_end   = 0x1000ULL - page_off;
            if (chunk > to_end) chunk = to_end;
        }
        copy_from_user(kbuf, (const uint8_t *)buf + done, (uint32_t)chunk);
        int n = ring_push(pair->input_buf, &pair->input_head,
                          pair->input_tail, PTY_BUF_SIZE,
                          kbuf, (uint32_t)chunk);
        if (n <= 0) break;
        done += (uint64_t)n;
    }
    return (int)done;
}

static void
master_close_fn(void *priv)
{
    pty_pair_t *pair = (pty_pair_t *)priv;
    pair->master_open = 0;
    /* If slave is also closed, release the pair */
    if (!pair->slave_open)
        pair->in_use = 0;
}

static void
master_dup_fn(void *priv)
{
    (void)priv; /* no ref counting needed — pty_pair_t is pool-allocated */
}

/* Master ioctl: TIOCGPTN (get pty number), TIOCSPTLCK (lock/unlock) */
#define TIOCGPTN   0x80045430UL
#define TIOCSPTLCK 0x40045431UL

static int
master_stat_fn(void *priv, k_stat_t *st)
{
    pty_pair_t *pair = (pty_pair_t *)priv;
    __builtin_memset(st, 0, sizeof(*st));
    st->st_mode  = S_IFCHR | 0600;
    st->st_ino   = 100 + pair->index;
    st->st_rdev  = makedev(5, 2); /* /dev/ptmx: major=5 minor=2 */
    st->st_dev   = 1;
    st->st_nlink = 1;
    return 0;
}

static const vfs_ops_t s_pty_master_ops = {
    .read    = master_read_fn,
    .write   = master_write_fn,
    .close   = master_close_fn,
    .readdir = (void *)0,
    .dup     = master_dup_fn,
    .stat    = master_stat_fn,
};

/* ── slave VFS ops ────────────────────────────────────────────────────── */

static int
slave_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
    (void)off;
    pty_pair_t *pair = (pty_pair_t *)priv;
    return tty_read(&pair->tty, (char *)buf, len);
}

static int
slave_write_fn(void *priv, const void *buf, uint64_t len)
{
    pty_pair_t *pair = (pty_pair_t *)priv;
    /* buf is a user pointer — copy to kernel first, then through tty_write */
    static char kbuf[256];
    uint64_t done = 0;
    while (done < len) {
        uint64_t chunk = len - done;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        {
            uint64_t page_off = (uint64_t)(uintptr_t)((const uint8_t *)buf + done) & 0xFFFULL;
            uint64_t to_end   = 0x1000ULL - page_off;
            if (chunk > to_end) chunk = to_end;
        }
        copy_from_user(kbuf, (const uint8_t *)buf + done, (uint32_t)chunk);
        int n = tty_write(&pair->tty, kbuf, chunk);
        if (n <= 0) break;
        done += (uint64_t)n;
    }
    return (int)done;
}

static void
slave_close_fn(void *priv)
{
    pty_pair_t *pair = (pty_pair_t *)priv;
    pair->slave_open = 0;
    if (!pair->master_open)
        pair->in_use = 0;
}

static void
slave_dup_fn(void *priv)
{
    (void)priv;
}

static int
slave_stat_fn(void *priv, k_stat_t *st)
{
    pty_pair_t *pair = (pty_pair_t *)priv;
    __builtin_memset(st, 0, sizeof(*st));
    st->st_mode  = S_IFCHR | 0620;
    st->st_ino   = 200 + pair->index;
    st->st_rdev  = makedev(136, pair->index); /* /dev/pts/N: major=136 */
    st->st_dev   = 1;
    st->st_nlink = 1;
    return 0;
}

static const vfs_ops_t s_pty_slave_ops = {
    .read    = slave_read_fn,
    .write   = slave_write_fn,
    .close   = slave_close_fn,
    .readdir = (void *)0,
    .dup     = slave_dup_fn,
    .stat    = slave_stat_fn,
};

/* ── public API ───────────────────────────────────────────────────────── */

int
ptmx_open(int flags, vfs_file_t *out)
{
    (void)flags;
    uint32_t i;
    for (i = 0; i < PTY_MAX_PAIRS; i++) {
        if (!s_pty_pool[i].in_use) {
            pty_pair_t *pair = &s_pty_pool[i];
            __builtin_memset(pair, 0, sizeof(*pair));
            pair->in_use = 1;
            pair->index  = (uint8_t)i;
            pair->locked = 1;  /* locked until unlockpt */
            pair->master_open = 1;
            pair->slave_open  = 0;
            tty_init_defaults(&pair->tty);
            pair->tty.write_out = pty_slave_write_out;
            pair->tty.read_raw  = pty_slave_read_raw;
            pair->tty.ctx       = (void *)pair;

            /* Controlling terminal: if opener is a session leader
             * with no controlling tty, acquire this one */
            {
                aegis_task_t *cur = sched_current();
                if (cur && cur->is_user) {
                    aegis_process_t *proc = (aegis_process_t *)cur;
                    if (proc->sid == proc->pid &&
                        !tty_find_controlling(proc->sid)) {
                        pair->tty.session_id = proc->sid;
                    }
                }
            }

            out->ops    = &s_pty_master_ops;
            out->priv   = (void *)pair;
            out->offset = 0;
            out->size   = 0;
            out->flags  = 0;
            out->_pad   = 0;
            return 0;
        }
    }
    return -12; /* ENOMEM — pool exhausted */
}

int
pts_open(uint32_t index, int flags, vfs_file_t *out)
{
    (void)flags;
    if (index >= PTY_MAX_PAIRS)
        return -2; /* ENOENT */
    pty_pair_t *pair = &s_pty_pool[index];
    if (!pair->in_use || !pair->master_open)
        return -2; /* ENOENT */
    if (pair->locked)
        return -13; /* EACCES — not yet unlocked */

    pair->slave_open = 1;

    /* Controlling terminal acquisition for session leader */
    {
        aegis_task_t *cur = sched_current();
        if (cur && cur->is_user) {
            aegis_process_t *proc = (aegis_process_t *)cur;
            if (proc->sid == proc->pid && pair->tty.session_id == 0)
                pair->tty.session_id = proc->sid;
        }
    }

    out->ops    = &s_pty_slave_ops;
    out->priv   = (void *)pair;
    out->offset = 0;
    out->size   = 0;
    out->flags  = 0;
    out->_pad   = 0;
    return 0;
}

tty_t *
pty_find_by_session(uint32_t session_id)
{
    uint32_t i;
    for (i = 0; i < PTY_MAX_PAIRS; i++) {
        if (s_pty_pool[i].in_use && s_pty_pool[i].tty.session_id == session_id)
            return &s_pty_pool[i].tty;
    }
    return (tty_t *)0;
}

int
pty_is_master(const vfs_file_t *f)
{
    return f->ops == &s_pty_master_ops;
}

int
pty_is_slave(const vfs_file_t *f)
{
    return f->ops == &s_pty_slave_ops;
}

tty_t *
pty_get_tty(const vfs_file_t *f)
{
    if (f->ops == &s_pty_slave_ops)
        return &((pty_pair_t *)f->priv)->tty;
    return (tty_t *)0;
}

/* tty_find_controlling — check console + all PTYs for session match */
tty_t *
tty_find_controlling(uint32_t session_id)
{
    if (session_id == 0) return (tty_t *)0;

    /* Check console */
    tty_t *con = tty_console();
    if (con && con->session_id == session_id)
        return con;

    /* Check PTY pool */
    return pty_find_by_session(session_id);
}
```

- [ ] **Step 3: Add pty.c to Makefile FS_SRCS**

After `kernel/fs/tty.c \`, add:
```
    kernel/fs/pty.c
```

- [ ] **Step 4: Build to verify**

Run: `rm -rf build && make`
Expected: Clean build.

- [ ] **Step 5: Commit**

```bash
git add kernel/fs/pty.h kernel/fs/pty.c Makefile
git commit -m "feat: add PTY pair infrastructure (16 pairs, master/slave ops)"
```

---

### Task 6: Wire PTY into VFS + ioctl

**Files:**
- Modify: `kernel/fs/vfs.c` (add /dev/ptmx and /dev/pts/ dispatch)
- Modify: `kernel/syscall/sys_file.c` (route ioctl for PTY fds)

- [ ] **Step 1: Add PTY dispatch to vfs_open**

In `kernel/fs/vfs.c`, add include:
```c
#include "pty.h"
```

In `vfs_open()`, before the `/proc/` check, add:
```c
    /* /dev/ptmx → allocate PTY master */
    if (path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v' && path[4]=='/' &&
        path[5]=='p' && path[6]=='t' && path[7]=='m' && path[8]=='x' && path[9]=='\0')
        return ptmx_open(flags, out);

    /* /dev/pts/N → open PTY slave */
    if (path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v' && path[4]=='/' &&
        path[5]=='p' && path[6]=='t' && path[7]=='s' && path[8]=='/') {
        uint32_t idx = 0;
        const char *s = path + 9;
        while (*s >= '0' && *s <= '9')
            idx = idx * 10 + (uint32_t)(*s++ - '0');
        if (*s != '\0') return -2; /* ENOENT — trailing garbage */
        return pts_open(idx, flags, out);
    }
```

Add /dev/ptmx and /dev/pts/ to vfs_stat_path. In the device special section, after the `/dev/null` block:
```c
    if (streq(path, "/dev/ptmx")) {
        __builtin_memset(out, 0, sizeof(*out));
        out->st_mode  = S_IFCHR | 0666;
        out->st_ino   = 6;
        out->st_rdev  = makedev(5, 2);
        out->st_dev   = 1;
        out->st_nlink = 1;
        return 0;
    }

    /* /dev/pts directory */
    if (streq(path, "/dev/pts")) {
        __builtin_memset(out, 0, sizeof(*out));
        out->st_dev   = 1;
        out->st_ino   = 7;
        out->st_nlink = 2;
        out->st_mode  = S_IFDIR | 0755;
        return 0;
    }

    /* /dev/pts/N — stat specific PTY slave */
    if (path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v' && path[4]=='/' &&
        path[5]=='p' && path[6]=='t' && path[7]=='s' && path[8]=='/') {
        __builtin_memset(out, 0, sizeof(*out));
        out->st_mode  = S_IFCHR | 0620;
        out->st_ino   = 8;
        out->st_rdev  = makedev(136, 0);
        out->st_dev   = 1;
        out->st_nlink = 1;
        return 0;
    }
```

- [ ] **Step 2: Update sys_ioctl to handle PTY fds**

In `kernel/syscall/sys_file.c`, add includes:
```c
#include "pty.h"
#include "tty.h"
```

In `sys_ioctl`, the current code checks `kbd_vfs_is_tty(f)` for termios ioctls. Extend to also check PTY slave fds. Replace the switch cases for TCGETS/TCSETS/TIOCSPGRP/TIOCGPGRP/TIOCGWINSZ:

Before the switch statement, add:
```c
    /* Determine if this fd has an associated tty_t */
    tty_t *tty = (tty_t *)0;
    if (kbd_vfs_is_tty(f))
        tty = tty_console();
    else if (pty_is_slave(f))
        tty = pty_get_tty(f);
```

Then update the ioctl cases to use `tty` instead of kbd_vfs functions:

```c
    case 0x5413UL: { /* TIOCGWINSZ */
        if (tty)
            return (uint64_t)(int64_t)tty_ioctl(tty, arg2, arg3);
        /* Fallback: hardcoded 25×80 for non-tty fds */
        uint16_t ws[4] = { 25, 80, 0, 0 };
        if (!user_ptr_valid(arg3, sizeof(ws)))
            return (uint64_t)-(int64_t)14;
        copy_to_user((void *)(uintptr_t)arg3, ws, sizeof(ws));
        return 0;
    }
    case 0x540FUL: { /* TIOCGPGRP */
        if (!tty) return (uint64_t)-(int64_t)25;
        return (uint64_t)(int64_t)tty_ioctl(tty, arg2, arg3);
    }
    case TCGETS: {
        if (!tty) return (uint64_t)-(int64_t)25;
        return (uint64_t)(int64_t)tty_ioctl(tty, arg2, arg3);
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        if (!tty) return (uint64_t)-(int64_t)25;
        return (uint64_t)(int64_t)tty_ioctl(tty, arg2, arg3);
    }
    case TIOCSPGRP: {
        if (!tty) return (uint64_t)-(int64_t)25;
        return (uint64_t)(int64_t)tty_ioctl(tty, arg2, arg3);
    }
```

For PTY master-specific ioctls, add before the default case:
```c
    case 0x80045430UL: { /* TIOCGPTN — get PTY number */
        if (!pty_is_master(f))
            return (uint64_t)-(int64_t)25;
        pty_pair_t *pair = (pty_pair_t *)f->priv;
        uint32_t n = (uint32_t)pair->index;
        if (!user_ptr_valid(arg3, sizeof(n)))
            return (uint64_t)-(int64_t)14;
        copy_to_user((void *)(uintptr_t)arg3, &n, sizeof(n));
        return 0;
    }
    case 0x40045431UL: { /* TIOCSPTLCK — lock/unlock PTY */
        if (!pty_is_master(f))
            return (uint64_t)-(int64_t)25;
        pty_pair_t *pair = (pty_pair_t *)f->priv;
        uint32_t val;
        if (!user_ptr_valid(arg3, sizeof(val)))
            return (uint64_t)-(int64_t)14;
        copy_from_user(&val, (const void *)(uintptr_t)arg3, sizeof(val));
        pair->locked = val ? 1 : 0;
        return 0;
    }
```

Also add TIOCSWINSZ support for the master (sets slave's window size + sends SIGWINCH):
```c
    case 0x5414UL: { /* TIOCSWINSZ */
        tty_t *ws_tty = tty;
        if (pty_is_master(f))
            ws_tty = &((pty_pair_t *)f->priv)->tty;
        if (!ws_tty) return (uint64_t)-(int64_t)25;
        return (uint64_t)(int64_t)tty_ioctl(ws_tty, arg2, arg3);
    }
```

- [ ] **Step 3: Build and test**

Run: `rm -rf build && make test`
Expected: PASS (no PTY consumers yet, console unchanged).

- [ ] **Step 4: Commit**

```bash
git add kernel/fs/vfs.c kernel/syscall/sys_file.c
git commit -m "feat: wire PTY into VFS open/stat and ioctl dispatch"
```

---

### Task 7: SIGHUP on Session Leader Exit

**Files:**
- Modify: `kernel/syscall/sys_process.c` (or `kernel/signal/signal.c`)

- [ ] **Step 1: Add SIGHUP delivery on session leader exit**

In the exit path — find where `sched_exit()` is called from `sys_exit` or `sys_exit_group`. Before the sched_exit call, add session leader cleanup:

Read `sys_process.c` to find sys_exit. Before the process is removed from the run queue, add:

```c
    /* Session leader exit: SIGHUP + SIGCONT to foreground group */
    if (proc->pid == proc->sid) {
        tty_t *ctty = tty_find_controlling(proc->sid);
        if (ctty && ctty->fg_pgrp) {
            signal_send_pgrp(ctty->fg_pgrp, 1 /* SIGHUP */);
            signal_send_pgrp(ctty->fg_pgrp, 18 /* SIGCONT */);
            ctty->session_id = 0; /* disassociate terminal */
        }
    }
```

Add include for tty.h at the top of the file if not already present.

- [ ] **Step 2: Build and test**

Run: `rm -rf build && make test`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add kernel/syscall/sys_process.c
git commit -m "feat: send SIGHUP on session leader exit"
```

---

### Task 8: PTY Test Binary + Integration Test

**Files:**
- Create: `user/pty_test/main.c`
- Create: `user/pty_test/Makefile`
- Create: `tests/test_pty.py`
- Modify: `tests/run_tests.sh`
- Modify: `Makefile` (DISK_USER_BINS + debugfs)

- [ ] **Step 1: Create user/pty_test/Makefile**

```makefile
CC      = musl-gcc
CFLAGS  = -static -O2 -fno-pie -no-pie -Wl,--build-id=none
TARGET  = pty_test.elf

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
```

- [ ] **Step 2: Create user/pty_test/main.c**

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

static volatile int got_sigint = 0;

static void sigint_handler(int sig)
{
    (void)sig;
    got_sigint = 1;
}

int main(void)
{
    /* Test 1: posix_openpt + grantpt + unlockpt + ptsname */
    int master = posix_openpt(O_RDWR);
    if (master < 0) {
        printf("PTY FAIL: posix_openpt\n");
        return 1;
    }
    if (grantpt(master) != 0) {
        printf("PTY FAIL: grantpt\n");
        return 1;
    }
    if (unlockpt(master) != 0) {
        printf("PTY FAIL: unlockpt\n");
        return 1;
    }
    char *name = ptsname(master);
    if (!name) {
        printf("PTY FAIL: ptsname returned NULL\n");
        return 1;
    }
    /* Verify it looks like /dev/pts/N */
    if (strncmp(name, "/dev/pts/", 9) != 0) {
        printf("PTY FAIL: ptsname='%s' (expected /dev/pts/N)\n", name);
        return 1;
    }

    /* Test 2: fork, child opens slave, writes, parent reads from master */
    pid_t pid = fork();
    if (pid < 0) {
        printf("PTY FAIL: fork\n");
        return 1;
    }
    if (pid == 0) {
        /* Child: open slave, write, exit */
        close(master);
        int slave = open(name, O_RDWR);
        if (slave < 0) _exit(1);
        write(slave, "hello\n", 6);
        close(slave);
        _exit(0);
    }

    /* Parent: read from master */
    char buf[64];
    int total = 0;
    /* Give child a moment to write */
    usleep(100000);
    int n = read(master, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        total = n;
    }

    int status;
    waitpid(pid, &status, 0);

    /* The slave output goes through line discipline (echo + ONLCR).
     * With OPOST+ONLCR, "hello\n" becomes "hello\r\n" in the output buffer.
     * Also, in canonical mode with echo, the slave's write is echoed back.
     * Actually: tty_write does output processing, so slave writing "hello\n"
     * goes through tty_write which pushes to output_buf (master reads it).
     * The output should contain "hello\r\n" (NL→CRNL from OPOST+ONLCR). */
    if (total <= 0 || !strstr(buf, "hello")) {
        printf("PTY FAIL: read from master got %d bytes: '%s'\n", total, buf);
        return 1;
    }

    close(master);
    printf("PTY OK\n");
    return 0;
}
```

- [ ] **Step 3: Create tests/test_pty.py**

Follow the exact pattern of `tests/test_mmap.py`. Same QEMU flags, login, run `/bin/pty_test`, check "PTY OK".

- [ ] **Step 4: Add to Makefile**

Add build rule:
```makefile
user/pty_test/pty_test.elf: user/pty_test/main.c
	$(MAKE) -C user/pty_test
```

Add to DISK_USER_BINS after proc_test:
```
	user/pty_test/pty_test.elf \
```

Add debugfs write command in the disk build printf:
```
write user/pty_test/pty_test.elf /bin/pty_test\n
```

- [ ] **Step 5: Add to run_tests.sh**

After the test_proc section:
```bash
echo "--- test_pty ---"
python3 tests/test_pty.py
```

- [ ] **Step 6: Build and run**

```bash
rm -rf build && make INIT=vigil iso && make disk
python3 tests/test_pty.py
```
Expected: `PASS: pty_test reported PTY OK`

- [ ] **Step 7: Run full test suite**

```bash
make test
```
Expected: PASS (boot.txt unchanged)

- [ ] **Step 8: Commit**

```bash
git add user/pty_test/main.c user/pty_test/Makefile \
        tests/test_pty.py tests/run_tests.sh Makefile
git commit -m "feat: add pty_test binary and test_pty.py integration test"
```

---

### Task 9: Update CLAUDE.md

**Files:**
- Modify: `.claude/CLAUDE.md`

- [ ] **Step 1: Update build status table**

Add row:
```
| TTY/PTY layer (Phase 32) | ✅ | Shared tty_t line discipline; 16 PTY pairs (/dev/ptmx + /dev/pts/N); sessions; setpgid relaxed; SIGTTIN/SIGTTOU; SIGHUP on session exit; test_pty.py PASS |
```

- [ ] **Step 2: Update roadmap**

Change Phase 32 from "Not started" to "✅ Done".

- [ ] **Step 3: Add Phase 32 forward constraints**

```markdown
## Phase 32 — Forward Constraints

**Phase 32 status: ✅ complete. `make test` passes. `test_pty.py` PASS.**

1. **PTY pool is static (16 pairs, ~138KB BSS).** No dynamic growth.

2. **No TIOCSTI (fake input injection).** Security risk. Deferred.

3. **No packet mode (TIOCPKT).** Not needed for SSH/screen. Deferred.

4. **No virtual consoles.** One console TTY + 16 PTYs.

5. **No SIGHUP on PTY master close.** Only session leader exit triggers SIGHUP. Master close → EIO on slave.

6. **SIGTTIN/SIGTTOU only on controlling terminal.** Background I/O to pipes/files unaffected.

7. **Console output still goes through printk.** The console tty's write_out calls printk("%c"). No direct framebuffer/serial write path for the tty layer.

8. **grantpt/unlockpt are no-ops in spirit.** grantpt returns 0. unlockpt clears a lock flag via ioctl. No ownership changes — capability system gates access.
```

- [ ] **Step 4: Update timestamp**

- [ ] **Step 5: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs: update CLAUDE.md for Phase 32 TTY/PTY completion"
```
