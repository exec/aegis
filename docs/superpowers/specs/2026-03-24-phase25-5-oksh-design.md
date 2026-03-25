# Phase 25.5 — oksh Port + TTY/Termios Layer Design

## Goal

Ship `/bin/oksh` (portable OpenBSD ksh) as the primary interactive shell on Aegis,
with real line editing, history, Ctrl-Z/fg/bg job control, and Ctrl-C/Ctrl-Z/Ctrl-\
signal delivery on both PS/2 and USB HID keyboards.

`/bin/sh` (current hand-written shell) is kept unchanged. `/bin/oksh` is the new
default launched by init. `/bin/whoami` is added as a side-effect.

---

## Architecture

Five areas of change, all self-contained:

```
kernel/arch/x86_64/kbd.c       — Ctrl signal delivery (PS/2 + USB HID)
kernel/fs/kbd_vfs.c            — termios raw/cooked mode switching
kernel/syscall/sys_file.c      — TCGETS/TCSETS/TIOCSPGRP in sys_ioctl
kernel/syscall/sys_process.c   — setpgid/setsid/getpgrp/getpgid/umask/getrlimit
kernel/proc/proc.h + proc.c    — pgid + umask fields in PCB
kernel/signal/signal.c         — SIGTSTP/SIGSTOP stop; SIGCONT resume
kernel/sched/sched.c           — PROC_STOPPED state; sched_stop/sched_resume
kernel/syscall/syscall.c       — new syscall dispatch entries
kernel/fs/initrd.c             — /dev/tty, /etc/passwd, /bin/oksh, /bin/whoami
user/oksh/                     — VPATH build, pconfig.h (patch approach)
user/whoami/                   — trivial binary
```

---

## Component Designs

### 1. kbd.c — Ctrl signal delivery

**Problem:** Ctrl-C, Ctrl-Z, Ctrl-\ do not deliver signals on USB HID keyboards
(ThinkPad). The PS/2 scancode path checks for Ctrl-C but `kbd_usb_inject` blindly
pushes every character to the ring buffer.

**Fix — PS/2 path:** Add two checks after the existing Ctrl-C block:
- Scancode 0x2C (z) while `s_ctrl` → send SIGTSTP to foreground pgroup
- Scancode 0x2B (\) while `s_ctrl` → send SIGQUIT to foreground pgroup

**Fix — USB HID path:** Add three intercepts at the top of `kbd_usb_inject`:
- `ascii == 0x03` (ETX, Ctrl-C) → send SIGINT to foreground pgroup, return
- `ascii == 0x1a` (SUB, Ctrl-Z) → send SIGTSTP to foreground pgroup, return
- `ascii == 0x1c` (FS, Ctrl-\) → send SIGQUIT to foreground pgroup, return

**`s_tty_pgrp`:** Replace `s_fg_pid` with `s_tty_pgrp` (process group ID). Signal
delivery uses `signal_send_pgrp(pgid, sig)` which iterates `s_procs[]`. The old
`sys_setfg` (syscall 360) is kept for backward compat with `/bin/sh` — it sets
`s_tty_pgrp` to a group containing only that PID (same effect as before since
`/bin/sh`'s children share its pgid until we teach it to call `setpgid`).

**New export:**
```c
void kbd_set_tty_pgrp(uint32_t pgid);
uint32_t kbd_get_tty_pgrp(void);
```

---

### 2. kbd_vfs.c — termios raw/cooked mode

**Stored state:**
```c
static struct termios s_termios;   /* kernel copy of terminal attributes */
static int            s_raw;       /* 1 = raw (oksh editor active)       */
```

Initialized with `c_lflag = ICANON | ECHO | ISIG`, `c_iflag = ICRNL`,
`c_cc[VMIN]=1`, `c_cc[VTIME]=0`.

`s_raw` is a cached bool updated atomically with every write to `s_termios`
inside `kbd_vfs_tcsets`. It is never an independent source of truth — it is
always re-derived as `!(s_termios.c_lflag & ICANON)` immediately after storing
the new termios. The read path checks `s_raw` directly (one load, no recompute
in the hot path).

**`kbd_vfs_read_fn` raw branch:**
When `s_raw`: call `kbd_read_interruptible(&interrupted)` and return the single
character immediately. No echo, no line buffer. oksh's emacs/vi editor handles
echo and display.

When cooked (current behaviour): unchanged — buffer until `\n`, echo as typed,
handle backspace.

**`kbd_stat_fn`:** Already returns `S_IFCHR`; no change needed.

**New functions called by sys_ioctl:**
```c
int  kbd_vfs_tcgets(void *dst_user);        /* copy s_termios to user   */
int  kbd_vfs_tcsets(const void *src_user);  /* copy from user, update s_raw */
```

Both validate the user pointer with `user_ptr_valid(ptr, sizeof(struct termios))`
before any copy. `kbd_vfs_tcsets` rejects a termios where `c_lflag` has impossible
flag combinations (reserved bits set) — returns EINVAL.

---

### 3. sys_file.c — sys_ioctl additions

New cases in the `switch (arg2)` block, all gated on the fd resolving to a
kbd/tty VFS file (checked via `f->ops == &s_kbd_ops`):

| ioctl | Number | Action |
|-------|--------|--------|
| TCGETS | 0x5401 | `kbd_vfs_tcgets(arg3_user)` |
| TCSETS | 0x5402 | `kbd_vfs_tcsets(arg3_user)` |
| TCSETSW | 0x5403 | `kbd_vfs_tcsets(arg3_user)` (drain is no-op) |
| TCSETSF | 0x5404 | `kbd_vfs_tcsets(arg3_user)` (flush is no-op) |
| TIOCSPGRP | 0x5410 | `user_ptr_valid(arg3, 4)` then `copy_from_user(&pgid, arg3, 4)`; `kbd_set_tty_pgrp(pgid)` |
| TIOCGPGRP | 0x540F | already exists, updated to use `kbd_get_tty_pgrp()` |

Non-kbd fds with TCGETS/TCSETS return `-ENOTTY` (unchanged default).

---

### 4. proc.h + proc.c — PCB additions

Two new fields added to `aegis_process_t`:

```c
uint32_t pgid;    /* process group ID; defaults to proc->pid on spawn */
uint32_t umask;   /* file creation mask; defaults to 022              */
```

`proc_spawn` initialises: `proc->pgid = proc->pid; proc->umask = 022;`

`proc_fork` copies both fields to the child (child inherits parent's pgid and
umask — correct POSIX behaviour; shell calls `setpgid` afterwards to create a
new group for the child).

---

### 5. signal.c + sched.c — stop/resume

**Existing scheduler state values** (from `sched.h`):
```c
#define TASK_RUNNING  0U
#define TASK_BLOCKED  1U
#define TASK_ZOMBIE   2U
```
Tasks are kept in a circular singly-linked list (`task->next`) rooted at
`s_current`. `sched_block` and `sched_wake` leave tasks in the list and just
flip the state; `sched_tick` skips non-`TASK_RUNNING` tasks with:
```c
do { s_current = s_current->next; } while (s_current->state != TASK_RUNNING);
```

**New state value:** Add `TASK_STOPPED 3U` to `sched.h`. No list removal needed
— `sched_tick` already skips any non-`TASK_RUNNING` state.

**New scheduler functions:**

```c
/* sched_stop — transition task to TASK_STOPPED.
 * If proc == s_current (self-stop on signal delivery): mirrors sched_block
 * exactly — sets state, advances s_current to next TASK_RUNNING task, updates
 * TSS/FS.base, calls ctx_switch.  Must be called with IF=0 (signal delivery
 * already runs with interrupts disabled or in syscall return path).
 * If proc != s_current (stopping a different task): simply set task->state =
 * TASK_STOPPED; sched_tick will skip it on the next preemption. */
void sched_stop(aegis_task_t *task);

/* sched_resume — transition TASK_STOPPED task back to TASK_RUNNING.
 * Mirrors sched_wake exactly: task->state = TASK_RUNNING.  No list
 * re-insertion needed (task was never removed).
 * If task is TASK_BLOCKED (received SIGCONT while blocked on a read),
 * also transitions to TASK_RUNNING so the blocking read can be interrupted. */
void sched_resume(aegis_task_t *task);
```

`sched_stop` self-stop path (prose of the implementation):
1. Set `task->state = TASK_STOPPED`
2. Advance `s_current` to next `TASK_RUNNING` task (same loop as `sched_block`)
3. Update `arch_set_kernel_stack` and `arch_set_fs_base` for incoming task
4. Call `ctx_switch(old, s_current)` — execution returns here when SIGCONT resumes
5. After resume: restore CR3 and FS.base for the resumed task (mirrors `sched_block` tail)

**signal.c default actions updated:**

| Signal | Old default | New default |
|--------|-------------|-------------|
| SIGTSTP | terminate | `sched_stop(proc)` |
| SIGSTOP | terminate | `sched_stop(proc)` (unblockable) |
| SIGCONT | terminate | `sched_resume(proc)`, deliver if handler set |
| SIGTTIN | terminate | `sched_stop(proc)` |
| SIGTTOU | terminate | `sched_stop(proc)` |

SIGSTOP and SIGCONT cannot be caught or ignored (SIG_DFL enforced even if
userspace calls `sigaction` with a handler — checked in `sys_rt_sigaction`).

**`waitpid` update:** When `options & WUNTRACED` and a child is PROC_STOPPED,
return `(stop_signum << 8) | 0x7f` immediately. The child remains stopped; a
subsequent `waitpid` with WCONTINUED would clear it (WCONTINUED is a no-op for
now — deferred to Phase 26).

---

### 6. sys_process.c — new syscalls

All new. All small.

**`sys_setpgid(pid, pgid)` — syscall 109:**
- If `pid == 0`: target = current process
- If `pid != 0`: look up proc; if not found → return `-ESRCH`; if found but not
  a direct child of current (`p->ppid != current->pid`) → return `-EPERM`
- If `pgid == 0`: `pgid = target->pid`
- If `pgid != target->pid` → return `-EPERM`
- Set `target->pgid = pgid`; return 0

**Aegis restriction (intentional POSIX deviation):** `pgid` must equal the
target's own PID. POSIX additionally allows joining an existing group within
the same session; Aegis does not implement sessions in Phase 25.5, so every
process can only be a group leader of its own singleton group. This is the safe
default — it prevents any process from injecting signals into an existing group
it didn't create. Real multi-process group joining is deferred to when session
support is added.

**`sys_getpgrp()` — syscall 111:** Return `current->pgid`.

**`sys_setsid()` — syscall 112:** Set `current->pgid = current->pid`. Return
`current->pid`. (No real session object yet; correct for our single-terminal use.)

**`sys_getpgid(pid)` — syscall 121:** Look up by pid, return `proc->pgid`. Returns
ESRCH if not found.

**`sys_umask(mask)` — syscall 95:** Store `mask & 0777` in `current->umask`.
Return previous value. (umask is not yet applied to file creation — ext2 create
path picks it up in Phase 26 when we add open-with-mode; the syscall itself is
needed now for oksh to call without crashing.)

**`sys_getrlimit(resource, rlim_user)` — syscall 97:** Validate user pointer
(`sizeof(struct rlimit)` = 16 bytes). Write `{RLIM_INFINITY, RLIM_INFINITY}`.
Return 0 for all resource values.

**`signal_send_pgrp(pgid, signum)` — new internal function:**
Walk the circular task list starting from `sched_current()` using the same
do-while pattern as `signal_send_pid`:
```c
aegis_task_t *cur = sched_current(); if (!cur) return;
aegis_task_t *t   = cur;
do {
    if (t->is_user) {
        aegis_process_t *p = (aegis_process_t *)t;
        if (p->pgid == pgid && p->pid != 1)
            p->pending_signals |= (1ULL << (uint32_t)signum);
            if (t->state == TASK_BLOCKED || t->state == TASK_STOPPED)
                sched_resume(t);   /* wake so it can deliver the signal */
    }
    t = t->next;
} while (t != cur);
```
PID 1 (init) is always excluded. SIGCONT additionally calls `sched_resume` on
any stopped/blocked targets so they can return to the run queue and deliver
the signal.

**`sys_kill` update — negative pid:** `if ((int64_t)arg1 < 0)` →
`signal_send_pgrp((uint32_t)(-(int64_t)arg1), sig)`.

---

### 7. initrd.c — new VFS entries

Three new file/device entries:

**`/dev/tty`:** Opens `s_kbd_file` (same singleton as stdin fd 0). `tty_init()` in
oksh calls `open("/dev/tty", O_RDWR)`, dup's it to a high fd, and calls
`tcgetattr`. All work with the kbd_vfs backend.

**`/etc/passwd`:** Static string embedded inline:
```
root:x:0:0:root:/:/bin/oksh
```
Registered as a read-only initrd file. Satisfies `getpwnam("root")` via musl's
`/etc/passwd` reader. The `~` expansion in oksh works.

**`/bin/oksh`:** Embedded as `kernel/oksh_bin.c` via `xxd -i`. Added to `s_files[]`
and `s_bin_entries[]`.

**`/bin/whoami`:** Embedded as `kernel/whoami_bin.c`. Added to both tables.

`s_dirs[]` entry for `/dev` added (alongside existing `/bin` and `/etc`).

---

### 8. user/oksh/ — VPATH patch build

Directory contains only:
- `Makefile`
- `pconfig.h`

No oksh source files are copied or modified. The Makefile uses `VPATH` to compile
directly from `../../references/oksh/`.

**Makefile:**
```makefile
CC      = musl-gcc
CFLAGS  = -static -O2 -s -fno-pie -no-pie -Wl,--build-id=none \
          -I. -I../../references/oksh
VPATH   = ../../references/oksh

SRCS = alloc.c asprintf.c c_ksh.c c_sh.c c_test.c c_ulimit.c edit.c \
       emacs.c eval.c exec.c expr.c history.c io.c jobs.c lex.c mail.c \
       main.c misc.c path.c shf.c syn.c table.c trap.c tree.c tty.c var.c \
       version.c vi.c confstr.c reallocarray.c siglist.c signame.c \
       strlcat.c strlcpy.c strtonum.c unvis.c vis.c issetugid.c

OBJS = $(SRCS:.c=.o)

oksh.elf: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

clean:
	rm -f *.o oksh.elf
```

**pconfig.h** defines exactly the features musl provides:
```c
#define HAVE_ASPRINTF
#define HAVE_REALLOCARRAY
#define HAVE_STRLCAT
#define HAVE_STRLCPY
#define HAVE_STRTONUM
#define HAVE_ST_MTIM
/* Not defined: HAVE_ISSETUGID HAVE_PLEDGE HAVE_CURSES HAVE_NCURSES */
/* Not defined: HAVE_STRAVIS HAVE_STRUNVIS HAVE_SIGLIST HAVE_SIGNAME */
/* Not defined: HAVE_TIMERADD HAVE_TIMERCLEAR HAVE_TIMERSUB */
/* Not defined: HAVE_SIG_T HAVE_SETRESUID HAVE_SETRESGID */
/* Not defined: HAVE_SRAND_DETERMINISTIC HAVE_CONFSTR */
```

If any oksh source file requires a patch (discovered at compile time), a
`aegis.patch` file is added and applied via `patch -p1 < aegis.patch` before
compilation.

---

### 9. user/whoami/

```c
#include <unistd.h>
int main(void) {
    /* getuid() returns 0; we have only root. */
    write(1, "root\n", 5);
    return 0;
}
```

Built with same musl-gcc flags. Could call `getpwuid(getuid())->pw_name` but that
requires `/etc/passwd` to be mounted — the direct write is simpler and always correct.

---

### 10. Makefile — build system wiring

New rules following the existing pattern:

```makefile
user/oksh/oksh.elf:
    $(MAKE) -C user/oksh

user/whoami/whoami.elf:
    $(MAKE) -C user/whoami

kernel/oksh_bin.c: user/oksh/oksh.elf
    cd user/oksh && xxd -i oksh.elf > ../../kernel/oksh_bin.c

kernel/whoami_bin.c: user/whoami/whoami.elf
    cd user/whoami && xxd -i whoami.elf > ../../kernel/whoami_bin.c
```

`oksh_bin.c` and `whoami_bin.c` added to `PROG_BIN_SRCS`. `user/oksh/oksh.elf` and
`user/whoami/whoami.elf` added to the `all` dependency chain.

---

## Security Notes

- All userspace pointer reads/writes use `user_ptr_valid` + `copy_from_user`/`copy_to_user`. No raw casts of syscall arguments to pointers.
- `setpgid` enforces: only self or direct child; pgid must equal target's own pid (cannot join an existing foreign group) — prevents signal-delivery escalation.
- `signal_send_pgrp` always skips PID 1 (init) regardless of its pgid field.
- SIGSTOP/SIGCONT cannot be caught or ignored — enforced in `sys_rt_sigaction`.
- Termios copy from user is size-checked to exactly `sizeof(struct termios)` before any write to kernel state.
- oksh runs with exactly the capability set inherited from init — no new capabilities granted.
- `/etc/passwd` is read-only in the initrd; no writable credential store exists.

---

## What Is Explicitly Not In This Phase

- Real POSIX sessions (`sid` field, `getsid()`, controlling terminal per session)
- `WCONTINUED` in waitpid
- History file persistence across reboots (works in memory per session)
- umask applied to file creation (syscall present, not yet wired to ext2 create)
- Tab completion requiring curses/ncurses
- Ctrl-D (EOF) on empty line — deferred (oksh exits on read returning 0; kbd_vfs returns EINTR on interrupt but not EOF yet)

---

## Test Plan

**Automated (`make test`):** All 15 existing tests must continue to pass.
No new boot oracle lines — oksh is launched at the interactive prompt level,
after all `[SUBSYSTEM] OK:` lines that the oracle matches. The oracle file
(`tests/expected/boot.txt`) is unchanged.

**New integration test (`tests/test_oksh.py`):** Python test modelled on
`test_net_stack.py`. Boots with `-machine q35`, sends keystrokes via a QEMU
monitor pipe or serial write, scans serial output for:
- `$ ` prompt (oksh interactive prompt)
- Response to `echo hello` → `hello` appears in serial output
- Response to `kill -TSTP $$` → process stops (SIGTSTP delivered)
The test kills QEMU when all expected strings are found or on timeout.

**Manual verification on ThinkPad:**
1. Boot → init launches `/bin/oksh` → `oksh` prompt appears
2. Type `ls` with arrow keys, backspace editing → line editor works
3. `sleep 10` then Ctrl-Z → `[1] + Stopped  sleep 10`; `fg` resumes it
4. `cat /dev/null` then Ctrl-C → process killed, prompt returns
5. `whoami` → prints `root`
6. `echo $PPID` → shows init PID
7. `/bin/sh` still works (launched manually)
