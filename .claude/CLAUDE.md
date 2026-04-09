# Aegis Kernel — Claude Code Persistent Instructions

> Read this file completely at the start of every session.
> Do not skip sections. If anything conflicts with a user instruction, follow the user and flag it.

---

## Bare-Metal Test Gate

**Strongly prefer bare-metal testing between phases.** QEMU hides real bugs (IOAPIC timing, AMD SS RPL, fork page copy performance). When hardware is available, test before starting the next phase.

However, this is a guideline, not a hard block. The catastrophic debugging session (2026-03-28) was caused by **stale git-tracked binaries surviving make clean**, not by implementing too many phases without testing. The nuclear clean build sequence is the actual non-negotiable — always use it for ISO creation. Productive work (bug fixes, cleanup, small features) can continue when hardware is unavailable.

## MANDATORY: Build Hygiene — Nuclear Clean Before Every ISO

**ALWAYS use this exact sequence when building an ISO for bare-metal testing:**

```bash
git clean -fdx --exclude=references --exclude=.worktrees
rm -f user/vigil/vigil user/vigictl/vigictl user/login/login.elf
make clean
make iso
```

**Why every step matters:**
1. `git clean -fdx` — removes ALL untracked files. `make clean` only removes `build/`.
2. `rm -f user/vigil/vigil ...` — these are GIT-TRACKED binaries. `git clean` doesn't touch them.
3. `make iso` — init is always vigil (hardcoded in Makefile).

**NEVER trust `make clean` alone. ALWAYS nuke user binaries.**
**NEVER use incremental builds for ISO creation.**

---

## What Aegis Is

A clean-slate, capability-based POSIX-compatible kernel. Not a Linux fork. Goal: boots real x86-64 hardware, runs real software via musl-compatible ABI, enforces a security model where no process — including root — holds ambient authority.

See MISSION.md for the full mission statement.

---

## Methodology — Superpowers Required

Every session must follow this order:

1. **Brainstorm before designing** — `/superpowers:brainstorm` for any non-trivial task.
2. **Plan before coding** — `/superpowers:write-plan` after brainstorm. Tasks ≤5 minutes each.
3. **Test before implementing** — RED before GREEN. Write failing test first. Watch it fail. Then implement. If you write code before a test exists, delete the code and start over.
4. **Review before merging** — code review skill after each task. Critical issues block progress.
5. **Verify before declaring done** — run `make test`, show output. "This should work" is not evidence.

If Superpowers is not installed, stop and tell the user.

---

## Architecture Rules — Non-Negotiable

### Directory Layout

```
kernel/arch/x86_64/   <- ALL x86-specific code. Nowhere else.
kernel/core/          <- Architecture-agnostic logic only. Must port to ARM64 tomorrow.
kernel/mm/            <- Memory management (arch-agnostic)
kernel/cap/           <- Capability subsystem (Rust)
kernel/fs/            <- VFS, filesystem drivers (ext2, initrd, ramfs, procfs, pipe, memfd)
kernel/tty/           <- Terminal line discipline + PTY pairs
kernel/drivers/       <- Hardware drivers (NVMe, xHCI, virtio-net, fb, etc.)
kernel/net/           <- Network stack (netdev_t, Ethernet, ARP, IP, TCP, UDP, ICMP)
kernel/proc/          <- Process creation, ELF loader
kernel/sched/         <- Scheduler
kernel/signal/        <- Signal delivery
kernel/syscall/       <- Syscall dispatch + implementations (split by domain)
tests/                <- Test harness and expected output
tools/                <- Build helpers, QEMU wrappers
docs/                 <- Architecture docs, audit findings, capability model spec
```

No x86 register names, port I/O, or MMIO addresses outside `kernel/arch/x86_64/`.

### The C/Rust Boundary

`kernel/cap/` and the syscall validation layer are Rust (`no_std`). Everything else is C. The boundary is a clean subsystem interface with a stable header — not sprinkled throughout the codebase. Crossed exactly at syscall entry and capability grant/revoke.

Rust rules:
- `no_std` always. `#![no_std]` at top of every file.
- No `alloc` without explicit sign-off.
- Every `unsafe` block: `// SAFETY: <specific reason>`. Vague comments are not acceptable.
- FFI functions: `#[no_mangle]` and documented.
- Clippy must pass. Clippy warnings are errors.

### Output Routing — The Two Laws

**Law 1:** All kernel output through `printk()`. Never write directly to `0xB8000` or serial registers. Pre-`printk` debug: use raw serial write functions in `serial.c` with a comment and TODO.

**Law 2:** Serial must always work even if VGA fails. `printk()` writes serial first, VGA second. If VGA fails, serial continues. If serial fails, panic. Never write code where VGA failure silences serial.

---

## Testing Harness

A subsystem is working when `make test` exits 0. Not when it compiles. Not when it looks right.

### How the harness works

`make test`:
1. Build kernel + GRUB-bootable ISO (exit on compiler warning — `-Werror` is set)
2. Boot headless:
   ```
   qemu-system-x86_64 -machine pc -cdrom aegis.iso -boot order=d \
     -display none -vga std -nodefaults -serial stdio -no-reboot \
     -m 128M -device isa-debug-exit,iobase=0xf4,iosize=0x04
   ```
3. Capture COM1; strip ANSI escape sequences **and `\r` carriage returns**; keep only lines starting with `[`
4. Diff against `tests/expected/boot.txt`
5. Exit 0 on exact match, exit 1 on any mismatch

**Note:** The boot oracle uses `-machine pc` (no virtio-net). Network initialization lines (`[NET] configured:`, `[NET] ICMP:`) only appear in q35 tests (test_net_stack.py). They must NOT appear in `tests/expected/boot.txt`.

**Why GRUB + ISO:** QEMU 10 dropped ELF64 multiboot2 detection via `-kernel`. GRUB is the standard multiboot2 loader.

**CRLF:** The serial driver emits `\r\n`. The harness sed must strip `\r` (`s/\r//g`) or diffs fail on every line.

### Serial output format

```
[SUBSYSTEM] OK: <message>
[SUBSYSTEM] FAIL: <reason>
```

`tests/expected/boot.txt` must contain the exact lines in order, no trailing spaces, single newline at end. Update it before writing the subsystem — that is the failing test.

### make targets

| Target       | What it does                                            |
|--------------|---------------------------------------------------------|
| `make`       | Build only                                              |
| `make run`   | Build + boot QEMU interactive (VGA + serial)           |
| `make test`  | Build + boot headless, diff serial output, exit 0 or 1 |
| `make clean` | Remove all build artifacts                              |
| `make sym ADDR=0x...` | Resolve address to source line (addr2line)    |
| `make gdb`   | Boot QEMU with GDB server on :1234, CPU halted          |
| `make disk`  | Build GPT disk image with ext2 partition                |

---

## Security Model

Capability-based, no ambient authority:
- Process starts with exactly the capabilities explicitly granted. Nothing more.
- `uid=0` is cosmetic. There is no superuser bypass.
- Capabilities are unforgeable tokens — not guessable integer IDs, not inherited by default.
- Syscalls requiring authority validate a capability token first. No valid token → `ENOCAP`.

`kernel/cap/` is a complete implementation. Read `docs/capability-model.md` before touching it. Never design a shortcut where "root can do it for now" — design the interface correctly even if the stub is minimal.

---

## Language and Style

**C:** K&R brace style. No dynamic allocation in early boot. No floating point, ever. No VLAs. No compiler extensions without `#ifdef` + comment. `-Wall -Wextra -Werror`. Explicit over implicit. No `malloc`/`free` — use PMM/VMM allocators.

**NASM:** Intel syntax. Explicit section names. Every function-like label documents what it does, what registers it clobbers, and its calling convention.

**Naming:** C functions `subsystem_verb_noun()`. Constants `SCREAMING_SNAKE_CASE`. Structs `aegis_subsystem_name_t` in C, `SubsystemName` in Rust. Files `lowercase_with_underscores.c`.

---

## Things You Must Never Do

1. Write directly to hardware outside the appropriate driver file.
2. Use `printf`, `malloc`, `free`, or any libc function in kernel code.
3. Assume x86 architecture in `kernel/core/`.
4. Mark `unsafe` without a `// SAFETY:` comment.
5. Declare a subsystem working without `make test` output.
6. Implement the capability system partially then move on. Correct stub or correct implementation — no in-between.
7. Add a dependency without flagging it explicitly to the user.
8. Suppress a compiler warning with a pragma without explaining why it's wrong in this specific case.
9. Write more than one subsystem at a time without a written plan.
10. Use `// TODO` as a substitute for thinking. If you don't know, say so.

---

## Build Toolchain

| Tool | Min version | Purpose |
|------|-------------|---------|
| `x86_64-elf-gcc` (→ `x86_64-linux-gnu-gcc` 14.2) | 12.x | Kernel C cross-compiler |
| `nasm` | 2.15 | Assembler |
| `ld` (x86_64-elf) | 2.38 | Linker |
| `rustup` + nightly + `rust-src` | latest | Rust cap/ subsystem |
| `qemu-system-x86_64` | 7.x+ | Emulator |
| `grub-mkrescue` (grub-pc-bin) | 2.06+ | Bootable ISO |
| `xorriso` | 1.5.x+ | Required by grub-mkrescue |
| `musl-gcc` (musl-tools) | 1.2.x+ | User binary cross-compiler |
| `sgdisk` (gdisk) | 1.0.x+ | GPT partition table |
| `e2tools` + `e2fsprogs` | any | ext2 image population (`make disk`) |

Run `x86_64-elf-gcc --version && nasm --version && rustup show && qemu-system-x86_64 --version && grub-mkrescue --version` before writing code. If any fail, stop.

---

## QEMU Boot Time

QEMU must boot to init prompt in under 30 seconds. If it takes longer, something is wrong — investigate before running tests. `BOOT_TIMEOUT=900` in test scripts is a CI safety net, not a target. Normal boots are 10-20 seconds.

Debug cycle: **one failure → diagnose → fix → one retry**. Do NOT retry the same failing test more than twice without changing something.

---

## Debug Tooling

CFLAGS includes `-g` and `-fno-omit-frame-pointer` for reliable stack unwinding.

**ASLR design constraint:** When ASLR is eventually implemented, debug builds MUST disable it so that `make sym ADDR=...` continues to resolve addresses deterministically.

**Panic backtrace:** `isr_dispatch` calls `panic_backtrace(s->rbp)` on kernel-mode exceptions (CS=0x08). Prints up to 16 return addresses. Ring-3 faults (CS=0x23) skip backtrace.

**Address resolution:** `make sym ADDR=0xffffffff80107abc` → wraps `addr2line -e build/aegis.elf -f -p`.

**GDB:** `make gdb` → QEMU with `-s -S`, GDB on :1234. Auto-connects via `tools/aegis.gdb`.
- `c` start, `Ctrl-C` pause, `bt` backtrace, `break isr_dispatch`, `p *s` (cpu_state_t), `x/20gx $rsp`

**Panic workflow:** serial output → `make sym` on RIP → `make sym` on each backtrace frame → if still unclear, `make gdb`.

---

## Build Status

A subsystem is ✅ only when `make test` passes with it included.

| Subsystem | Status | Notes |
|-----------|--------|-------|
| Boot (multiboot2) | ✅ | GRUB + GRUB ISO; identity-mapped at 0x100000 |
| Serial (COM1) | ✅ | 115200 baud 8N1; emits `\r\n` for terminals |
| VGA text mode | ✅ | 80x25, ROM font |
| printk | ✅ | serial-first, VGA-conditional; `%u` for unsigned, no `%d` |
| Test harness | ✅ | GRUB ISO + ANSI-strip + CRLF-strip + diff |
| PMM | ✅ | Bitmap allocator; 4KB pages; no contiguous multi-page |
| VMM + paging | ✅ | Higher-half at 0xFFFFFFFF80000000; identity map removed |
| KVA allocator | ✅ | Bump allocator; `kva_free_pages` unmaps but VA not reclaimed |
| SMAP + uaccess | ✅ | CR4.SMAP; `copy_from_user`/`copy_to_user`; EFAULT on bad ptr |
| IDT + PIC + PIT | ✅ | 48 vectors; 8259A remapped; 100 Hz timer |
| PS/2 keyboard | ✅ | Scancode→ASCII; ring buffer; `kbd_read()` + `kbd_poll()` |
| Scheduler | ✅ | Preemptive round-robin; single-core; `ctx_switch` in NASM |
| GDT + TSS + SYSCALL/SYSRET | ✅ | Ring-3 descriptors; RSP0; IA32_STAR/LSTAR/SFMASK |
| SMEP + SMAP | ✅ | CR4.SMEP + CR4.SMAP; graceful WARN if CPU lacks support |
| ELF loader + User process | ✅ | PT_LOAD → user PML4; per-process PML4+kernel stack; iretq |
| Syscall dispatch | ✅ | sys_write/read/open/close/exit/brk/mmap/fork/execve/waitpid/... |
| Capability system | ✅ | Rust `CapSlot`; `cap_grant`/`cap_check` FFI; per-process caps[] |
| VFS + initrd | ✅ | vfs.h; initrd (18 files); console/kbd/pipe VFS devices |
| Shell + musl | ✅ | musl-gcc static binary; fork/execve/waitpid; interactive shell |
| Pipes + redirection | ✅ | `sys_pipe2`/`dup`/`dup2`; ring buffer; `|`, `<`, `>`, `2>&1` |
| Signals | ✅ | sigaction/sigprocmask/sigreturn/kill/setfg; Ctrl-C |
| PCIe + ACPI + NVMe | ✅ | MCFG+MADT on q35; ECAM ≤8 buses; NVMe 1.4 blkdev |
| ext2 filesystem | ✅ | Read-write; 16-slot LRU block cache; double indirect; symlinks |
| xHCI + USB HID | ✅ | xHCI on q35+qemu-xhci; keyboard + mouse HID boot protocol |
| GPT partitions | ✅ | CRC32 + primary header; Aegis GUID prefix |
| virtio-net + net stack | ✅ | Ethernet/ARP/IPv4/ICMP/TCP/UDP; netdev_t; 12-byte virtio header |
| Vigil init system | ✅ | Service supervision + respawn |
| Socket API + epoll | ✅ | socket/bind/listen/accept/connect/send/recv; sock_t VFS fds; httpd |
| DHCP + curl | ✅ | Userspace RFC 2131; BearSSL+curl static; outbound TCP fixed in Phase 43a |
| Threads (Phase 29) | ✅ | clone(CLONE_VM); per-thread TLS; fd_table_t refcount; futex |
| mprotect + mmap (Phase 30) | ✅ | W^X via NX/EFER.NXE; 64-slot VA freelist |
| /proc (Phase 31) | ✅ | VMA tracking; procfs VFS; CAP_KIND_PROC_READ |
| TTY/PTY (Phase 32) | ✅ | 16 PTY pairs; sessions; SIGTTIN/SIGTTOU; SIGHUP |
| Dynamic linking (Phase 33) | ✅ | PT_INTERP+ET_DYN; MAP_FIXED+file-backed mmap; musl libc.so |
| Writable root (Phase 34) | ✅ | ext2-first VFS; rootfs.img multiboot2 module; RAM blkdev |
| Installer (Phase 35) | ✅ | Text-mode; GPT+GRUB+rootfs copy; UEFI boot |
| USB HID mouse (Phase 36) | ✅ | /dev/mouse VFS; boot protocol; xHCI device type detection |
| Lumen compositor (Phase 37) | ✅ | Backbuffer composite; z-order windows; PTY terminal; taskbar |
| SMP (Phase 38) | ✅ | LAPIC+IOAPIC; ~30 spinlocks; SWAPGS; AP trampoline; TLB shootdown |
| Glyph + optimization (Phase 39) | ✅ | libglyph.a widget toolkit; dirty-rect compositor |
| Citadel + sys_spawn (Phase 40) | ✅ | sys_spawn (514); lumen terminal via spawn; desktop icons |
| Symlinks + chmod/chown (Phase 41) | ✅ | ext2 symlinks; DAC enforcement |
| stsh — Styx shell (Phase 42) | ✅ | CAP_DELEGATE/QUERY; caps/sandbox builtins; line editing; tab completion |
| Quiet boot + lumen fixes (Phase 42b) | ✅ | printk_quiet; dual-ISO test harness |
| IPC (Phase 44) | ✅ | AF_UNIX SOCK_STREAM; SCM_RIGHTS; memfd_create; MAP_SHARED |
| capd + sys_cap_grant (Phase 45) | ✅ Retired | capd daemon + sys_cap_grant(363) removed in Phase 46c cap-policy redesign |
| Bastion (Phase 46) | ✅ | Graphical display manager; libauth.a + libcitadel.a |
| GUI polish (Phase 46b) | ✅ | Dark mode, frosted glass, TTF fonts, sys_reboot |
| Cap-policy redesign (Phase 46c) | ✅ | Two-tier kernel policy; /etc/aegis/caps.d/; sys_auth_session(364); capd+exec_caps eliminated |

### Known deviations

| Item | Spec | Actual | Reason |
|------|------|--------|--------|
| `x86_64-elf-gcc` | native cross | symlink → `x86_64-linux-gnu-gcc` 14.2 | Not in Debian repos; equivalent with `-ffreestanding -nostdlib` |
| Boot method | QEMU `-kernel` | GRUB + `-cdrom` | QEMU 10 dropped ELF64 multiboot2 via `-kernel` |
| Serial isolation | direct diff | strip ANSI + strip CRLF + `grep ^[` + diff | SeaBIOS/GRUB contaminate COM1; serial driver uses CRLF |
| QEMU test flags | `-nographic` | `-display none -vga std` | GRUB falls back to serial without a VGA device |
| Ring-3 SS selector | 0x1B (RPL=3) | 0x18 (RPL=0) on AMD | AMD 64-bit mode strips SS RPL bits; `(ss & ~3) == 0x18` check accepts both |

---

## Session Startup Checklist

- [ ] Read MISSION.md
- [ ] Read this file — all of it
- [ ] Run `make test` — current baseline?
- [ ] Check Build Status table above
- [ ] Confirm Superpowers is active
- [ ] Ask the user what the goal for this session is
- [ ] Do not write code until brainstorm and plan are complete

---

## Phase Roadmap

| Phase | Content | Status |
|-------|---------|--------|
| 25-46b | (All complete — see Build Status table) | ✅ Done |
| 46c | **Cap-policy redesign** — two-tier kernel policy (service+admin caps); /etc/aegis/caps.d/; sys_auth_session(364); capd+exec_caps+sys_cap_grant_exec/runtime removed | ✅ Done |
| 47 | **GUI installer** — graphical version of text-mode installer using Glyph | Not started |
| 48 | **Super key + extended keyboard** — PS/2 E0; Super modifier; multimedia scancodes | Not started |
| 49 | **TCP polish** — send segmentation; per-connection TX buffer; flow control. Required for SSH. | Not started |
| 50 | **TinySSH + sftp-server** — NaCl crypto, key-only auth; CAP_KIND_NET_LISTEN | Not started |
| 51 | **Timers** — setitimer/alarm/timerfd; POSIX interval timers | Not started |
| 52 | **virtio-blk driver** — PCI virtio block (1af4:1001); needed for cloud VMs | Not started |
| 53 | **Deep security audit** — syscall fuzzing, capability bypass, TOCTOU, SMP atomicity | Not started |
| 54 | **Release** | Not started |
| — | HDA audio — Intel HDA controller; PCM playback/capture; /dev/audio VFS | Post-release |
| — | RTL8125 2.5GbE driver — post-release, requires WiFi confirmed working | Post-release |

---

## Aegis GPT Partition Type GUIDs — Installer Reference

**This section is permanent project documentation. Do not condense, summarize, or remove during cleanup.**

Aegis uses a custom GPT partition type GUID family to identify its own disk partitions. This prevents the kernel from accidentally mounting an incompatible ext2 (or other) volume — a critical safety property since the fallback is volatile RAM storage.

### GUID Structure

```
A3618F24-0C76-4B3D-xxxx-xxxxxxxxxxxx
 \___ Aegis prefix ___/  \__ role __/
```

**Prefix bytes (hex):** `A3 61 8F 24 0C 76 4B 3D`

### Defined Partition Roles

| Full GUID | Role | Description |
|-----------|------|-------------|
| `A3618F24-0C76-4B3D-0001-000000000000` | Root | Primary ext2 root filesystem. Mounted as `/`. |
| `A3618F24-0C76-4B3D-0002-000000000000` | Swap | Swap partition. |
| `A3618F24-0C76-4B3D-0003-000000000000` | Home | User home directories (`/home`). |
| `A3618F24-0C76-4B3D-0004-000000000000` | Data | General-purpose persistent data. |

Role `0001` (Root) is the only one implemented. The others are reserved.

### How It Works

1. **`make disk` / installer:** Creates GPT partitions with `sgdisk --typecode=1:A3618F24-0C76-4B3D-0001-000000000000`.
2. **`gpt_scan()` in kernel:** Compares type GUID against 8-byte Aegis prefix. Non-matching partitions silently skipped.
3. **`ext2_mount("nvme0p1")`:** Only succeeds if registered by `gpt_scan` (Aegis-prefixed).
4. **Safety:** Non-Aegis partitions (e.g., Linux installs) are invisible. Falls back to ramdisk.

---

## Aegis GUI Architecture — Lumen/Glyph/Citadel

**This section is permanent project documentation. Do not condense, summarize, or remove during cleanup.**

Aegis has its own native GUI stack — no X11, no Wayland, no bloat. The framebuffer is mapped directly into userspace via `sys_fb_map` (syscall 513).

### Components

| Codename | Role | Linux Equivalent | Description |
|----------|------|-----------------|-------------|
| **Lumen** | Display server + compositor | Wayland compositor | Owns the framebuffer. Composites client windows. Dispatches events. |
| **Glyph** | Widget toolkit library | GTK/Qt | `libglyph.so` — buttons, labels, text input, window chrome, layout. |
| **Citadel** | Desktop shell | GNOME Shell / KDE Plasma | Taskbar, app launcher, desktop icons, clock. |
| **Bastion** | Display manager | GDM / SDDM | Graphical login + lock screen. Owns session lifecycle. |

### Architecture (v0.2 — Phase 46)

Bastion owns the graphical session. Vigil starts Bastion (not Lumen). Bastion authenticates, then spawns Lumen as a child. Citadel is `libcitadel.a` linked into Lumen. Auth is `libauth.a` shared between `/bin/login` (text) and `/bin/bastion` (graphical).

```
Bastion (display manager) → authenticates → spawns Lumen
  Lumen (compositor) — links libcitadel.a + libglyph.a
    Terminal, Terminal, ... (PTY children via sys_spawn)
  Win+L → SIGUSR1 to Bastion → lock screen
```

**Target architecture (future):** Lumen becomes `liblumen.a`. Bastion links everything into one binary. No framebuffer handoff.

### Font

**Terminus** bitmap font (SIL OFL 1.1): `ter-u20n` (10x20) — 96 columns x 54 rows at 1080p. Embedded as C array.

### Design Principles

1. No protocol overhead — direct framebuffer compositor.
2. One toolkit — Glyph is THE widget library.
3. Dynamically linked — `libglyph.so`.
4. Developer-friendly — `#include <glyph.h>`, link with `-lglyph`.
5. Capability-gated — framebuffer access requires capabilities.

---

## Active Constraints — Still Relevant

These are constraints from completed phases that remain load-bearing. Grouped by topic.

### Outbound TCP — BROKEN (Phase 40b)

`curl -sk https://example.com` exits with rc=7 (ECONNREFUSED). Inbound TCP works (test_socket PASS). Outbound connect through SLIRP NAT has a race condition — `tcp_send_segment` outside `tcp_lock` races with `tcp_tick`. Next steps: TCP debug logging or `make gdb` breakpoints in tcp_connect/tcp_rx.

### IPC Limits (Phase 44)

- UNIX_SOCK_MAX = 32 (16 concurrent connections)
- MEMFD_PAGES_MAX = 2048 (8MB — one 1080p BGRA framebuffer)
- CAP_KIND_IPC = 15 is the last cap slot. CAP_TABLE_SIZE = 16.
- No SOCK_DGRAM AF_UNIX, no abstract namespace, no MSG_PEEK/DONTWAIT/WAITALL
- MAP_SHARED only for memfd fds (not ext2 or pipes)
- ftruncate only for memfd (not ext2)

### Capability System (Phase 46c — policy redesign)

Capabilities use a two-tier kernel policy model. Policy files in `/etc/aegis/caps.d/<binary>` declare caps per-binary. The kernel reads these at execve time.

- **Service caps** (unconditional): granted to any process that execs the binary. For daemons and standard tools.
- **Admin caps** (require authenticated session): only granted if `proc->authenticated` is set. `sys_auth_session` (syscall 364) sets this flag after login verifies credentials.
- **Baseline** (hardcoded, all processes): VFS_OPEN, VFS_READ, VFS_WRITE, IPC, PROC_READ(read), THREAD_CREATE.
- **Retired syscalls**: `sys_cap_grant_exec` (361) and `sys_cap_grant_runtime` (363) removed. capd daemon eliminated. Vigil `exec_caps` mechanism eliminated.
- No cap revocation. Restart process to reset caps.
- `ls /` OOM is pre-existing since Phase 45.

### SMP (Phase 38)

- Single-core scheduling only. APs start but enter halt loops.
- SWAPGS omitted from proc_enter_user (AMD Zen 2 workaround).
- IOAPIC remapped PIC vectors 0xF0-0xFF have silent-drop ISR stubs.

### Kernel Limits

- Kernel BSS must stay under 6MB (vmm_init maps pd_hi[0..2]).
- PTY pool: 16 pairs, static. No virtual consoles.
- ioctl request codes compared as uint32_t (musl sign-extension).
- Identity map covers 0-1GB (boot.asm pd_lo 512 entries).
- Module memory (~60MB) permanently reserved; ramdisk copies it (~60MB more).
- No ASLR. Interpreter at fixed INTERP_BASE (0x40000000).
- No COW fork. Full page copy. MMIO pages skipped, batch yield every 32 pages.

### Filesystem

- VFS open order: initrd first → ext2 fallback.
- Symlinks only on ext2. Depth limit 8. No hard links.
- Permission enforcement only on ext2. ramfs/initrd/procfs use capabilities.
- No uid=0 bypass (by design). umask not applied to ext2_create.
- Boot-time file opens bypass DAC (`is_user` check).

### Networking

- Shared static TX buffers (no concurrent sends).
- Polling at 100 Hz via PIT. No MSI/MSI-X.
- ARP from ISR context returns -1 (no blocking). TCP retransmit handles it.
- DHCP is oneshot (vigil does not restart on crash).

### stsh (Phase 42)

- No scripting, command substitution, glob expansion, or job control.
- stsh is ext2-only. Login falls back to /bin/sh.
- sys_spawn has 5 parameters (cap_mask is 5th).

---

## Architecture Audit (2026-03-29) — Status (updated 2026-04-09)

### HIGH — Fix Soon

| ID | Issue | File | Status |
|----|-------|------|--------|
| C5 | **SYSRET signal delivery saves incomplete registers** — callee-saved (rbx, rbp, r12-r15) corrupted after signal handler. ISR path correct. | signal.c:365 | ✅ **FIXED** — save path signal.c:365-375, restore path sys_signal.c:147-152. Bonus X1 (non-canonical RIP reject) and X2 (RFLAGS sanitization) hardening in sys_rt_sigreturn. |

### MEDIUM — Track and Fix

| ID | Issue | File | Status |
|----|-------|------|--------|
| M1 | **sched_wake has no memory barrier** — safe on x86, broken on ARM64. | sched.c:358 | ✅ **FIXED** — `__atomic_store_n(&task->state, TASK_RUNNING, __ATOMIC_RELEASE)`. |
| M2 | **mmap freelist has no lock for CLONE_VM threads** — safe single-core, corruption on SMP. | sys_memory.c | **TODO** |
| M3 | **vmm_window_lock > pmm_lock ordering fragile** — undocumented. | vmm.c | Document |

### Performance Recommendations (Future)

| ID | Issue | Impact | Effort |
|----|-------|--------|--------|
| P1 | **COW fork** | Eliminates fork stall | Large |
| P3 | **Separate run queue** (RUNNING-only list) | Scheduler scalability | Medium |
| P4 | **draw_px optimization** | Faster GUI rendering | Small |
| WQ | **Wait queue abstraction** (`waitq_t`) | Cleaner blocking I/O | Medium |

### ARM64 Port Status

**Build: BROKEN.** xhci.c GCC 15 array-bounds fail. 8 .o files missing from Makefile. `smp_percpu_init_bsp()` not called. Minimum fix ~2 hours. Remaining: TTBR0/TTBR1, PAN, vmm_free_user_pages, Rust cap cross-compile.

### Lock Ordering (Canonical)

```
sched_lock > (all others)
vmm_window_lock > pmm_lock > kva_lock
tcp_lock before sock_lock (deferred wake pattern)
ip_lock before arp_lock (copy-then-release pattern)
```

---

**RTL8125 testing note:** Dev machine has RTL8125B (ASUS, PCI 0a:00.0, IOMMU group 18) managed by host `r8169`. WiFi is MT7921K (0b:00.0). Do NOT test RTL8125 until WiFi is confirmed working — you will lose remote access.

**Test hardware:**
- **Dev machine:** ASUS Ryzen 7 6800H, RTL8125B, Wi-Fi MT7921K
- **Test machine A:** Ryzen 7 6800H, dual RTL8168, Intel AX200 Wi-Fi, Samsung NVMe
- **Test machine B (ThinkPad X13 Gen 1):** Ryzen 7 Pro 4750U (Zen 2 / Renoir)
- **AMD SS RPL (FIXED):** AMD 64-bit mode strips SS RPL bits (ss=0x18 vs 0x1B). `idt.c` accepts both via `(ss & ~3) == 0x18` + RPL normalization.

---

## REMINDER: Bare-Metal Testing

**Strongly prefer bare-metal testing between phases.** QEMU hides real bugs. But don't block productive work when hardware is unavailable. The nuclear clean build sequence is the actual non-negotiable for ISOs.
