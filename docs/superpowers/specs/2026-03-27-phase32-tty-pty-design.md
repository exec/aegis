# Phase 32: TTY/PTY Layer — Design Spec

## Goal

Add a proper TTY abstraction with shared line discipline, pseudo-terminal (PTY) support (16 pairs via `/dev/ptmx` + `/dev/pts/N`), session leaders with controlling terminals, and correct job control (SIGTTIN/SIGTTOU, SIGHUP on session exit). Refactor existing console/keyboard terminal logic into the shared TTY layer so console and PTYs use identical line discipline code.

## Architecture

### TTY Abstraction (`tty_t`)

A `tty_t` represents a terminal device — either the physical console or a PTY slave. It owns the line discipline state (termios, canonical buffer, echo) and delegates raw I/O to a backend.

```c
typedef struct tty {
    k_termios_t termios;
    uint32_t    fg_pgrp;       /* foreground process group */
    uint32_t    session_id;    /* controlling session (0 = no session) */
    uint16_t    rows, cols;    /* window size */
    char        linebuf[512];  /* canonical mode line buffer */
    uint32_t    line_len;      /* bytes in linebuf */
    uint8_t     line_ready;    /* newline received, data available */

    /* Backend callbacks */
    int  (*write_out)(struct tty *tty, const char *buf, uint32_t len);
    int  (*read_raw)(struct tty *tty, char *out, int *interrupted);
    void *ctx;                 /* backend-specific: NULL for console, pty_pair_t* for PTY */
} tty_t;
```

**Shared line discipline operations (tty.c):**
- `tty_read(tty, buf, len)` — canonical or raw mode read with echo, backspace, Ctrl-D, signal generation (Ctrl-C → SIGINT to `tty->fg_pgrp`, Ctrl-Z → SIGTSTP, Ctrl-\ → SIGQUIT)
- `tty_write(tty, buf, len)` — output processing (OPOST: NL→CRNL if enabled)
- `tty_ioctl(tty, cmd, arg)` — TCGETS, TCSETS, TCSETSW, TCSETSF, TIOCGPGRP, TIOCSPGRP, TIOCGWINSZ, TIOCSWINSZ

### Two Backends

**Console backend** (singleton, boot-time):
- `write_out` → `printk` (serial + VGA)
- `read_raw` → `kbd_read_interruptible` (PS/2 / USB HID ring buffer)
- The existing console. Functionally identical behavior after refactor.

**PTY slave backend** (per-pair, allocated on `/dev/ptmx` open):
- `write_out` → push bytes to `output_buf` (master reads them)
- `read_raw` → pull bytes from `input_buf` (master writes them), block if empty
- Signal generation via line discipline in `tty_read`, not in kbd.c.

### PTY Pair Structure

```c
typedef struct {
    uint8_t  input_buf[4096];    /* master→slave ring buffer */
    uint32_t input_head, input_tail;
    uint8_t  output_buf[4096];   /* slave→master ring buffer */
    uint32_t output_head, output_tail;
    tty_t    tty;                /* slave's TTY (termios, line discipline, fg_pgrp) */
    uint8_t  master_open;
    uint8_t  slave_open;
    uint8_t  locked;             /* cleared by unlockpt (TIOCSPTLCK) */
    uint8_t  in_use;
    uint8_t  index;              /* 0-15 */
} pty_pair_t;
```

**Static pool:** `static pty_pair_t s_pty_pool[16]` (~8.6KB per pair, ~138KB total BSS).

### PTY Allocation Flow

```
posix_openpt(O_RDWR)           →  open("/dev/ptmx")
  → ptmx_open()                →  allocates pty_pair_t from pool
  → returns master fd           →  vfs_ops = s_pty_master_ops

grantpt(master_fd)              →  ioctl(fd, TIOCGPTN, &n) (musl impl)
  → no-op, returns 0            →  capability system gates access

unlockpt(master_fd)             →  ioctl(fd, TIOCSPTLCK, &zero)
  → clears pair->locked

ptsname(master_fd)              →  ioctl(fd, TIOCGPTN, &n)
  → returns "/dev/pts/<n>"

open("/dev/pts/N")              →  pts_open(N)
  → fails if pair->locked       →  unlockpt must be called first
  → returns slave fd             →  vfs_ops = s_pty_slave_ops (backed by tty_read/write)
```

### Master VFS Ops

The master fd is NOT a TTY — it's the "other end" of the pipe:
- `master_read()` → pull from `output_buf` (reads what slave wrote)
- `master_write()` → push to `input_buf` (becomes slave's input, processed by line discipline)
- `master_ioctl()` → TIOCGPTN (get PTY number), TIOCSPTLCK (lock/unlock), TIOCSWINSZ (set window size → SIGWINCH to fg_pgrp)
- `master_close()` → set `master_open = 0`; if slave reads, return EIO (hangup)

### Slave VFS Ops

The slave fd IS a TTY:
- `slave_read()` → `tty_read(&pair->tty, ...)` (line discipline processes input_buf)
- `slave_write()` → `tty_write(&pair->tty, ...)` (output processing, pushes to output_buf)
- `slave_ioctl()` → `tty_ioctl(&pair->tty, ...)` (termios, pgrp, winsize)
- `slave_close()` → set `slave_open = 0`; if master reads, return 0 (EOF)

### Session Leaders + Controlling Terminal

**New field in `aegis_process_t`:**
```c
uint32_t sid;    /* session ID; == pid for session leaders */
```

**`setsid()` implementation:**
- If `proc->pid == proc->pgid` (already a group leader, might be session leader): return `-EPERM`
- Set `proc->sid = proc->pid`, `proc->pgid = proc->pid`
- Detach from controlling terminal (no TTY association for this session yet)
- Return new session ID

**Controlling terminal acquisition:** When a session leader (sid == pid) opens a slave PTY (or the console TTY) and has no controlling terminal, that TTY becomes the session's controlling terminal: `tty->session_id = proc->sid`.

**Lookup:** To find a process's controlling terminal, scan the tty pool (console + 16 PTYs) for `tty->session_id == proc->sid`. Max 17 checks — fast enough.

**`setpgid()` relaxation:** Currently restricted to `pgid == pid` (singleton groups only). Relax to allow:
- `setpgid(child, child)` — create new group (existing behavior)
- `setpgid(child, existing_pgid)` — join existing group in same session
- Validation: target pgid must belong to a process in the same session

### SIGTTIN / SIGTTOU

When a background process (pgid != tty->fg_pgrp) calls:
- `tty_read()` → send SIGTTIN to the caller's process group, return `-EINTR`
- `tty_write()` with TOSTOP set in termios → send SIGTTOU, return `-EINTR`

This suspends the background process until it's brought to the foreground.

### SIGHUP on Session Leader Exit

When a process exits and `proc->pid == proc->sid` (session leader):
- Find the session's controlling terminal
- Send SIGHUP then SIGCONT to the foreground process group
- Disassociate the terminal from the session (`tty->session_id = 0`)

### Signal Generation Refactor

Currently, `kbd.c` detects Ctrl-C/Z/\ and calls `signal_send_pid(s_tty_pgrp, SIGINT/SIGTSTP/SIGQUIT)`. This moves into `tty_read()` in the line discipline:

- When `tty_read` processes input and sees Ctrl-C (with ISIG set in c_lflag):
  - `signal_send_pgrp(tty->fg_pgrp, SIGINT)`
  - Discard the character
- Same for Ctrl-Z → SIGTSTP, Ctrl-\ → SIGQUIT

`kbd.c` becomes a raw character source: it pushes ASCII into the console `tty_t`'s input path. No signal logic in kbd.c.

For the console backend, `kbd_handler()` pushes chars into the ring buffer. `tty_read` calls `read_raw` which calls `kbd_read_interruptible` to pull from the ring buffer. The line discipline in tty.c handles signal generation.

For PTY slaves, the master writes to `input_buf`. `tty_read` calls `read_raw` which pulls from `input_buf`. Same line discipline handles signals.

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `kernel/fs/tty.h` | Create | tty_t struct, tty_read/write/ioctl, tty_init_defaults |
| `kernel/fs/tty.c` | Create | Shared line discipline, signal generation, SIGTTIN/SIGTTOU |
| `kernel/fs/pty.h` | Create | pty_pair_t, ptmx_open, pts_open, PTY pool |
| `kernel/fs/pty.c` | Create | PTY pair management, master/slave VFS ops, ring buffers |
| `kernel/fs/kbd_vfs.c` | Modify | Replace inline line discipline with tty_read/write delegation |
| `kernel/fs/console.c` | Modify | console_write becomes write_out callback for console tty |
| `kernel/fs/vfs.c` | Modify | Add /dev/ptmx and /dev/pts/ dispatch in vfs_open + vfs_stat_path |
| `kernel/arch/x86_64/kbd.c` | Modify | Remove signal generation; become raw char source only |
| `kernel/proc/proc.h` | Modify | Add sid field to aegis_process_t |
| `kernel/proc/proc.c` | Modify | Initialize sid in proc_spawn |
| `kernel/syscall/sys_process.c` | Modify | Fix setsid, relax setpgid, SIGHUP on session leader exit |
| `kernel/syscall/sys_file.c` | Modify | Route ioctl for PTY fds through tty_ioctl |
| `kernel/signal/signal.c` | Modify | Add SIGHUP delivery on session leader exit |
| `Makefile` | Modify | Add tty.c and pty.c to FS_SRCS |
| `user/pty_test/main.c` | Create | Test binary: posix_openpt/grantpt/unlockpt/ptsname/fork+slave |
| `user/pty_test/Makefile` | Create | musl-gcc static build |
| `tests/test_pty.py` | Create | Integration test |
| `tests/run_tests.sh` | Modify | Add test_pty.py |

## Testing

**Test binary `user/pty_test/main.c`:**
1. `posix_openpt(O_RDWR)` → master fd (succeeds)
2. `grantpt(master)` → 0
3. `unlockpt(master)` → 0
4. `ptsname(master)` → "/dev/pts/0" (verify string)
5. Fork: child opens slave, writes "hello\n", exits
6. Parent reads from master, verifies "hello\n" appears
7. Test signal: parent writes `\x03` (Ctrl-C) to master; child installs SIGINT handler, verifies it fires
8. Print "PTY OK" on success

**Integration test `tests/test_pty.py`:** Same q35+NVMe pattern. Boot, login, run `/bin/pty_test`, check "PTY OK".

**boot.txt:** Unchanged — no init line for TTY subsystem.

**`make test`:** Must still pass — console behavior unchanged after refactor.

## Forward Constraints

1. **PTY pool is static (16 pairs, ~138KB BSS).** No dynamic growth.

2. **No TIOCSTI.** Fake input injection is a security risk. Deferred.

3. **No packet mode (TIOCPKT).** Not needed for SSH/screen. Deferred.

4. **Console refactor is behavior-preserving.** The console path must behave identically after moving line discipline to tty.c. `make test` is the regression gate.

5. **Signal generation moves from kbd.c to tty.c.** kbd.c becomes a raw character source.

6. **No virtual consoles.** One console TTY + 16 PTYs.

7. **SIGTTIN/SIGTTOU for controlling terminal only.** Background I/O to pipes/files is unaffected.

8. **No SIGHUP on PTY master close yet.** Only session leader exit triggers SIGHUP. Master fd close → EIO on slave read (simpler, correct for v1).
