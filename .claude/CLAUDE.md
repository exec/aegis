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
kernel/arch/x86_64/   <- x86-64-specific code (bulk — boot, IDT/GDT/TSS, LAPIC, IOAPIC, ...)
kernel/arch/arm64/    <- ARM64-specific code (boot, GIC, PL011, exception vectors, ...)
kernel/core/          <- Cross-arch logic. Arch-specific branches allowed via #ifdef dispatch.
kernel/mm/            <- Memory management (cross-arch with #ifdef dispatch where unavoidable)
kernel/cap/           <- Capability subsystem (Rust, no_std, portable)
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

**Arch isolation rule (revised 2026-04-12):** Bulk arch-specific code lives in `kernel/arch/<arch>/`. Cross-arch files (`kernel/syscall/`, `kernel/signal/`, `kernel/proc/`, etc.) may use `#ifdef __x86_64__` / `#ifdef __aarch64__` inline dispatch where the code is inherently arch-specific (saved register structs, frame accessors, TLS register reads, syscall translation tables, ELF machine recognition). Keeping related arch branches colocated is more readable than scattering them across parallel arch-specific files.

**Still not allowed in cross-arch files:**
- Hardcoded x86 register names/MMIO addresses without an `#ifdef` guard (e.g., `inb(0x64)` unconditionally in `main.c`)
- x86 function signatures leaking into shared headers (e.g., `panic_bluescreen(rip, cr2, rsp, ...)` — use a neutral `isr_frame *`)
- Unconditional use of x86-only constants like `0xFFFFFFFF80000000` — wrap in `KERN_VA_BASE` macro defined per-arch
- Direct `#include "arch/x86_64/*.h"` from cross-arch files — go through a stable arch interface header

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
| GUI installer + Lumen external window protocol (Phase 47) | ✅ | gui-installer is a real Lumen window via AF_UNIX protocol (memfd+SCM_RIGHTS); LUMEN_OP_CREATE_WINDOW/DAMAGE/DESTROY/CREATE_PANEL/INVOKE; mouse + Enter dispatch; CAP_KIND_FB no longer required |
| Citadel dock split (Phase 47b) | ✅ | /bin/citadel-dock is its own process; talks to Lumen over the protocol; vigil graphical-mode service. First step of subsystem peeling |
| 1.0.2 — Esc + arrow nav + chronos cap (Apr 17) | ✅ | PS/2 ESC scancode fix; gui-installer arrow nav; Lumen Ctrl+Alt+I + LUMEN_OP_INVOKE "gui-installer"; chronos NET_SOCKET cap; stbtt assertion no-op; bastion `bastion_autologin=USER` test hook; deflaked GUI installer test 0/15 → 15/15 |
| 1.0.3 — coreutils + envp + sethostname + GRUB-on-installed (Apr 17/18) | ✅ | 20 new coreutils (sleep/head/tail/basename/dirname/tee/env/date/hostname/sync/tr/cut/expand/realpath/stat/yes/test/[/find/which/uniq); kernel envp propagation in sys_execve; sys_sethostname (170) + persistent hostname; installed-system grub.cfg fix (loadfont, gfxmode auto, ESP-relative paths); installer existing-Aegis-install warning UX; SIMPLE_USER_PROGS Makefile dep fix; stsh strerror diagnostic; coreutils_test scaffold; assert_cmd helper. 11/20 utils CI-asserted; 9 deferred to 1.0.4 (kernel ext2 cache ENOENT after ~11 sequential ext2 execs + one expand-specific exec mystery). |

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
| 47 | **GUI installer** — graphical version of text-mode installer using Glyph | ✅ Done (v1.0.1) |
| 47b | **Subsystem peeling** — split Lumen subsystems into separate binaries; dock done. Terminal next, then taskbar/file-manager. | Dock done; terminal queued |
| 1.0.2 | **Esc fix + arrow nav + Ctrl+Alt+I + chronos cap + deflake** | ✅ Done (v1.0.2, Apr 17) |
| 1.0.3 | **20 coreutils + envp/sethostname kernel patches + installed-GRUB fix + installer overwrite warning + SIMPLE_USER_PROGS dep fix + stsh strerror** | ✅ Done (v1.0.3, Apr 18) |
| 48 | **Super key + extended keyboard** — PS/2 E0; Super modifier; multimedia scancodes | **Active** |
| 1.0.4 (parked) | **kernel ext2 ENOENT after ~11 sequential ext2 execs (re-enables 9 deferred coreutils); /bin/expand single-binary exec mystery; pre-existing lumen test failures (Tasks 26/27/29/30)** | Pending |
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

### Architecture (v0.3 — Phase 47/47b)

Bastion owns the graphical session. Vigil starts Bastion. Bastion auths, spawns Lumen. Lumen runs the AF_UNIX server at `/run/lumen.sock`. External processes (citadel-dock, gui-installer) are first-class clients via the **external window protocol**. Terminal + widget_test are still in-process Lumen built-ins (next to peel).

```
Bastion (display manager) → auths → spawns Lumen
  Lumen (compositor) — links libglyph.a; serves /run/lumen.sock
    Terminal, Terminal, ... (PTY children via sys_spawn)
  Win+L → SIGUSR1 to Bastion → lock screen

  ↕ AF_UNIX (external window protocol)

  /bin/citadel-dock  (panel, vigil graphical-mode service)
  /bin/gui-installer (regular window, on-demand)
```

**External window protocol** (`user/lib/glyph/lumen_proto.h`, `lumen_client.{c,h}`, `user/bin/lumen/lumen_server.{c,h}`):
- Handshake: `lumen_hello_t` magic+version, reply with status
- Opcodes (client→server): `LUMEN_OP_CREATE_WINDOW`, `LUMEN_OP_CREATE_PANEL` (chromeless, non-focusable, bottom-anchored), `LUMEN_OP_DAMAGE`, `LUMEN_OP_DESTROY_WINDOW`, `LUMEN_OP_INVOKE` (ask Lumen to spawn a built-in by name — currently "terminal" / "widgets")
- Events (server→client): `LUMEN_EV_KEY`, `LUMEN_EV_MOUSE`, `LUMEN_EV_CLOSE_REQUEST`, `LUMEN_EV_FOCUS`, `LUMEN_EV_RESIZED` (reserved, never sent in v1)
- Memfd-backed pixel buffer passed via `SCM_RIGHTS` on the CREATE reply. Client renders to backbuf, copies to shared, sends DAMAGE.
- `lumen_window_created_t` includes `width, height, x, y` — clients need x,y for screen-relative coordinates (e.g. `[DOCK]` debug lines)
- Lumen single-byte keys go through `focused->on_key(byte)` if set (terminals' `on_key` writes to PTY master_fd; proxy windows' `on_key` sends `LUMEN_EV_KEY`). Falls back to direct `tag` write for legacy paths. Multi-byte ESC sequences (arrows / CSI) still PTY-only — known gap.
- `LUMEN_RUNNING=1` env guard: `/bin/lumen` exits with "you're already using lumen, pal" if launched recursively.
- `lumen-probe` (`user/bin/lumen-probe/`) is a test/diagnostic binary — connects, creates a 200x100 blue window, presents, destroys, exits. Used by `tests/tests/lumen_proxy_uaf_test.rs`. NOT auto-launched in production rootfs.

**Cleanup invariants** (UAF-class bugs to not regress):
- `comp_remove_window(comp, win)` already calls `glyph_window_destroy(win)`. Do NOT call `glyph_window_destroy` again afterward in proxy hangup paths — double free.
- `comp_remove_window` clears `c->focused`, `c->drag_win`, `c->content_drag_win` if they pointed to the removed window. Anything else holding a `glyph_window_t *` outside the windows[] array MUST be cleared by `comp_remove_window` (or audited on add/raise paths).
- Proxy windows: `pw->shared` is munmap'd in hangup. After hangup, the focused/drag pointers must already be cleared or they'll fault on the next event with CR2 in the mmap region.
- Close button: invokes `win->on_close()` if set (proxy windows use this to forward `LUMEN_EV_CLOSE_REQUEST` to the owner instead of self-destruct), otherwise falls back to `comp_remove_window`.

**glyph_window_t.tag** initializes to **-1** (sentinel). Default 0 from calloc collided with valid PTY fd 0 in dispatch checks. Lumen uses `tag >= 0` to mean "deliver via direct PTY write".

**Target architecture (future):** Continue subsystem peeling. Terminal next, then taskbar, file manager, settings. Each becomes its own binary using the protocol; Lumen shrinks toward pure compositor + AF_UNIX broker.

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

### Outbound TCP — WORKING (fixed, verified 2026-04-15)

Outbound TCP works. `nettest` vigil service connects to `1.1.1.1:80`, sends HTTP GET, receives 255 bytes — verified on QEMU q35 + SLIRP. The `tcp_send_segment` / `tcp_lock` race described in the Phase 40b note has been resolved. `curl -sk https://example.com` works from the shell.

### IPC Limits (Phase 44)

- UNIX_SOCK_MAX = 32 (16 concurrent connections)
- **UNIX_BUF_SIZE = 4096** — must stay a power of two. The ring math uses `(head - tail) & (UNIX_BUF_SIZE - 1)` which only computes a real modulo for power-of-two sizes. Original 4056 was broken (`8 & 4055 == 0`); fixed in v1.0.1.
- MEMFD_PAGES_MAX = 2048 (8MB — one 1080p BGRA framebuffer)
- CAP_KIND_IPC = 15 is the last cap slot. CAP_TABLE_SIZE = 16.
- No SOCK_DGRAM AF_UNIX, no abstract namespace, no MSG_PEEK/DONTWAIT/WAITALL
- MAP_SHARED only for memfd fds (not ext2 or pipes). `sys_mmap` MAP_SHARED size check compares `len > page_count * 4096` (page-rounded), not `len > mf->size` — fixed in v1.0.1, sub-page-aligned ftruncate works.
- AF_UNIX `fcntl(F_SETFL, O_NONBLOCK)` propagates to `unix_sock_t.nonblocking` (sys_file.c). AF_UNIX has a real `.poll` callback (`unix_vfs_poll`); checks accept queue (LISTENING), peer ring (CONNECTED), peer state (POLLHUP). Fixed in v1.0.1; both required to unfreeze Lumen on bare metal.
- AF_UNIX bind does NOT create a filesystem entry — the path lives in an in-kernel name table (`name_register`/`name_lookup`). Clients must retry connect until ECONNREFUSED stops; `stat()` will not work.

### Polling and wake-up (Phase 47c — per-fd waitqs)

- All pollable fds have a `waitq_t poll_waiters` (or shared global for kbd/console/mouse). Producers (writers, ISRs, state-change paths) call `waitq_wake_all` on the object's queue.
- `sys_poll` and `sys_epoll_wait` register one `waitq_entry_t` per watched fd (stack-allocated, max 64 fds for poll / `EPOLL_MAX_WATCHES` for epoll). Plus `g_timer_waitq` if timeout is finite.
- Removed: global `g_poll_waiter` and the PIT wake hack at `pit.c:80-84` that wrote to it. PIT now calls `waitq_wake_all(&g_timer_waitq)`.
- New file: `kernel/syscall/fd_waitq.{c,h}` — single-point `fd_get_waitq(int fd)` dispatch (AF_INET → AF_UNIX → vfs_ops_t.get_waitq → NULL).
- Lock order: waitq locks are leaf. `sched_lock` > waitq lock. Producers call `waitq_wake_all` after dropping any object-specific lock (sock_lock, tcp_lock, etc.) to avoid `sched_lock` reentry under those locks.
- Spec: `docs/superpowers/specs/2026-04-16-poll-waitq-design.md`. Plan: `docs/superpowers/plans/2026-04-16-poll-waitq-implementation.md`. Regression test: `tests/tests/poll_concurrent_pollers_test.rs`.

### Capability System (Phase 46c — policy redesign)

Capabilities use a two-tier kernel policy model. Policy files in `/etc/aegis/caps.d/<binary>` declare caps per-binary. The kernel reads these at execve time.

- **Service caps** (unconditional): granted to any process that execs the binary. For daemons and standard tools.
- **Admin caps** (require authenticated session): only granted if `proc->authenticated` is set. `sys_auth_session` (syscall 364) sets this flag after login verifies credentials.
- **Baseline** (hardcoded, all processes): VFS_OPEN, VFS_READ, VFS_WRITE, IPC, PROC_READ(read), THREAD_CREATE.
- **Retired syscalls**: `sys_cap_grant_exec` (361) and `sys_cap_grant_runtime` (363) removed. capd daemon eliminated. Vigil `exec_caps` mechanism eliminated.
- No cap revocation. Restart process to reset caps.
- **Every binary that calls `socket()` needs `NET_SOCKET` in its policy file.** Missing-policy = baseline-only = `socket()` returns EPERM. chronos shipped without one for two phases (silent failure: `chronos: socket failed` every boot) — fixed in 1.0.2 by adding `rootfs/etc/aegis/caps.d/chronos`. If you add a new daemon that needs network, write the cap file at the same time.

### v1.0.2 invariants — DO NOT REGRESS

- **PS/2 ESC scancode** (`kernel/arch/x86_64/kbd.c`): `s_sc_lower[0x01]` and `s_sc_upper[0x01]` MUST be `'\033'`, not `0`. The translation loop's `if (c)` filter drops NULs, so a zero entry silently swallows every Esc keypress system-wide. This bug was latent from Phase 9 until 1.0.2.
- **stbtt assertion** (`user/lib/glyph/font.c`): `STBTT_assert(x)` is `#define`d to `((void)0)` before including `stb_truetype.h`. The upstream `dy >= 0` assert in `stbtt__fill_active_edges_new` fires intermittently on certain glyphs and `abort()`s the compositor. Worst case of skipping is one bad scanline; do not re-enable.
- **Lumen invoke + Ctrl+Alt+I** (`user/bin/lumen/main.c`): `invoke_handler` dispatches `"terminal"`, `"widgets"`, `"gui-installer"`. `Ctrl+Alt+I` (escape-prefix `eb == 0x09`) calls `invoke_handler(comp, "gui-installer")`. The `spawn_external_client()` helper uses `sys_spawn` with `stdio_fd_arg=2` so client diag prints reach `/dev/console`.
- **Arrow keys for proxy windows** (`user/bin/lumen/main.c` CSI handler): when focused window has `tag < 0` (proxy) and the CSI sequence is 3 bytes, translate `ESC [ A/B/C/D` to synthetic single-byte codes `0xF1`/`0xF2`/`0xF3`/`0xF4` and dispatch via `on_key`. PTY-backed windows (`tag >= 0`) get the raw sequence written to their PTY as before. Both paths must coexist.
- **gui-installer arrow nav** (`user/bin/gui-installer/main.c`): `KEY_ARROW_LEFT (0xF4)` → `handle_back()`, `KEY_ARROW_RIGHT (0xF3)` → `handle_key('\r')`. Esc still works as back. Must keep both because Esc is what Lumen sends for bare ESC and arrows produce the synthetic codes.
- **gui-installer lumen_connect retry** (`user/bin/gui-installer/main.c` main): retries on `-ECONNREFUSED` (-111) up to 50 times with 100ms sleep. Aegis AF_UNIX returns ECONNREFUSED until `accept()` runs, so first-connect can race compositor startup.
- **stsh background jobs** (`user/bin/stsh/{parser,exec}.c`): `cmd &` is real now. `parse_pipeline_bg` strips trailing `&` and sets a flag; `run_pipeline_bg` forks but doesn't `waitpid`. SIGCHLD=SIG_IGN in stsh main means children auto-reap.
- **Bastion autologin retry** (`user/bin/bastion/main.c`): `bastion_autologin=USER` on `/proc/cmdline` skips the greeter and authenticates with hardcoded "forevervigilant". Calls `do_auth()` up to 5 times with 200ms sleep — there's a libauth race in cold boot that we don't fully understand yet (TODO). Production interactive login is untouched. Production ISOs never set the cmdline arg.
- **Test ISO** (`tools/grub-installer-test.cfg`, `make installer-test-iso`): `aegis-installer-test.iso` has same rootfs as `aegis.iso` but kernel cmdline contains `bastion_autologin=root`. Tests use this; production never ships it.
- **Makefile rootfs deps**: `$(ROOTFS)` now depends on `find rootfs -type f` so editing `/etc` files actually triggers a rebuild. Before 1.0.2 you had to `rm -f build/rootfs.img` by hand.

### v1.0.3 invariants — DO NOT REGRESS

- **envp propagation in `sys_execve`** (`kernel/syscall/sys_exec.c`): the new envp copy block reuses the existing `env_bufs[32][256]`/`env_ptrs[33]`/`env_str_ptrs[32]` fields in `execve_argbuf_t` (originally added for sys_spawn in Phase 40). DO NOT add parallel `envp_*` fields — duplicates 8 KB of working storage and creates two paths in one struct. Stack layout: env strings pushed at HIGHER VAs than argv strings; pointer table is `argc, argv[…], NULL, envp[…], NULL, auxv[…]`. The `table_qwords = argc2 + envc + 3 + auxv_qwords` math at line 396 must include envc or alignment breaks.
- **`sys_sethostname` (170)** (`kernel/syscall/sys_hostname.c`): gated by `cap_check(CAP_KIND_POWER, CAP_RIGHTS_READ)` returning `-1` (EPERM) on failure — matches `sys_reboot` precedent (the existing user of CAP_KIND_POWER) and `sys_fb_map`. Returning ENOCAP would leak a non-POSIX errno to userspace via musl's wrapper. Lock (`s_hostname_lock`) is a leaf — no other locks taken under it; cap_check and sched_current happen BEFORE acquisition. Buffer `s_hostname[65]` lives in `.data`, init "aegis", `len > HOSTNAME_MAX(64)` rejected before any write. `sys_uname` (`kernel/syscall/sys_identity.c`) MUST source nodename from `hostname_get()`, not hardcode it again.
- **GRUB on installed system** (`tools/grub-installed.cfg`): three load-bearing fixes — (1) NO `if … then true … fi` (no `true` builtin in grub-mkimage's BOOTX64.EFI), use unconditional `loadfont` calls instead. `||`/`&&` also don't work. (2) `loadfont` and `background_image` MUST run BEFORE `search --set=root` reassigns `$root`, with absolute `/EFI/BOOT/…` paths (the ESP, where mcopy actually puts these files). (3) `set gfxmode=…,auto` — the trailing `auto` is required because OVMF + std-vga only offers 24-bpp GOP modes which the kernel rejects.
- **Installer existing-install detection** (`user/lib/libinstall/copy.c`, `user/bin/gui-installer/main.c`): `install_disk_has_aegis(devname)` walks the kernel block-device list looking for `<devname>pN` children. `gpt_scan` only registers Aegis-typed partitions, so a partition-child = an existing Aegis install. Disk-select screen renders such disks in orange with `[existing Aegis install]`; confirm screen swaps the orange "all data will be erased" banner for the red two-line "this disk already contains an Aegis install" warning.
- **`SIMPLE_USER_PROGS` Makefile rule** (`Makefile`): rule MUST list `$(wildcard user/bin/$(1)/*.c) $(wildcard user/bin/$(1)/*.h) user/bin/$(1)/Makefile` as prereqs, not just `$(MUSL_BUILT)`. Without this, editing `user/bin/<name>/main.c` does not invalidate the `.elf` and the rootfs.img silently packages a stale binary. Caught when stsh edits weren't taking effect during 1.0.3.
- **stsh strerror diag** (`user/bin/stsh/exec.c`): `execve` failure now prints `<argv0>: <strerror(errno)>` instead of unconditional "not found". Requires `#include <errno.h>`. Don't revert — this is how we diagnose kernel-side exec failures (Task 29 was caught with it).
- **Kernel ext2 ENOENT after ~11 sequential ext2-backed `execve`s — KNOWN BUG (Task 29).** Initrd-served binaries (cat/ls/echo/sh/login/vigil) are unaffected. New ext2-only coreutils (env/sync/stat/yes/test/[/find/which/uniq/expand) trip the bug after enough exec'd from one boot. Suspected: 16-slot LRU block cache evicting indirect blocks, cache-miss path returning ENOENT instead of refilling. Investigate `kernel/fs/ext2.c` (`ext2_open`/`ext2_read_block`/eviction handler). Defers 9 of the 20 1.0.3 coreutils from CI verification.
- **Test scaffold** (`tests/tests/coreutils_test.rs`): boots `aegis-test.iso` (text mode), waits for `[STSH] ready` marker (emitted from `user/bin/stsh/main.c` AFTER all stsh init, BEFORE the REPL loop, NEVER in `-c` mode), then drives utils via `assert_cmd(proc, stream, cmd, &[expected], label)`. Helper sentinel matching uses `l.trim() == sentinel`, NOT `l.contains(sentinel)` — substring matched on stsh's local-echo of the typed command line. Atomic counter for back-to-back calls.
- **Test ISO env var:** `AEGIS_TEST_ISO` (text-mode) for `coreutils_test`. `AEGIS_INSTALLER_TEST_ISO` (graphical autologin) for `gui_installer_test`. Don't conflate.

### Vortex test harness (local-only changes, not in vortex repo yet)

These live in `~/Developer/vortex/src/core/qemu.rs` on the dev box:
- `cmd.kill_on_drop(true)` on QEMU spawn — leaked test panics no longer leave stranded QEMU processes.
- Stale monitor socket cleanup before bind — `let _ = std::fs::remove_file(&path);` immediately before passing `unix:{path},server,nowait`.
- `serial_capture: true` now sets `stderr(Stdio::null())` instead of `Stdio::piped()` — piped+unread stderr fills (~64K) and blocks QEMU mid-boot.
- `VORTEX_SERIAL_TEE=/path/to/file` env var: appends every raw serial line to a file, useful when `wait_for_line` consumes the line you wanted to debug.
- `send_keys` map extended with: `&` `_` `:` `;` `=` `!` `@` `#` `$` `%` `^` `*` `(` `)` `\x1b` (1.0.2) plus `|` `\` `'` `"` `>` `<` `?` `[` `]` `+` (1.0.3, around line 338, after the `)` → `shift-0` mapping). Without them `coreutils_test` can't drive shell pipes/redirections from the harness.
- These should be upstreamed to vortex eventually (Task 27).

### CI workflow notes

- `.github/workflows/build-iso.yml`: x86 `build-iso` job and ARM64 `build-arm64` job are independent (no `needs:`). The ISO builds and the GitHub release is created from build-iso even if build-arm64 fails.
- Cross-toolchain symlink loops MUST include `ranlib` (added 1.0.2). build-arm64-userland.sh calls aarch64-elf-ranlib.
- build-arm64 needs nightly + `aarch64-unknown-none` for kernel/cap. Don't forget to add the rust-src component.

### SMP (Phase 38)

- Single-core scheduling only. APs start but enter halt loops.
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
| M2 | **mmap freelist has no lock for CLONE_VM threads** — safe single-core, corruption on SMP. | sys_memory.c | ✅ **FIXED** — `spinlock_t mmap_free_lock` in `proc.h:43`; `mmap_free_insert`/`mmap_free_alloc` both take it with `spin_lock_irqsave`. Fork/clone initialize it. |
| M3 | **vmm_window_lock > pmm_lock ordering fragile** — undocumented. | vmm.c | ✅ **FIXED** — documented inline at vmm.c:54-69 with cross-reference to the global Lock Ordering section in this file. |

### Performance Recommendations (Future)

| ID | Issue | Impact | Effort |
|----|-------|--------|--------|
| P1 | **COW fork** | Eliminates fork stall | Large |
| P3 | **Separate run queue** (RUNNING-only list) | Scheduler scalability | Medium |
| P4 | **draw_px optimization** | Faster GUI rendering | Small |
| WQ | **Wait queue abstraction** (`waitq_t`) | Cleaner blocking I/O | Medium |

### ARM64 Port Status (2026-04-12)

**Build: WORKING.** `make -C kernel/arch/arm64` produces `build/aegis-arm64.elf` (1.1MB). Boots on `qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 2G -kernel build/aegis-arm64.elf -nographic` through to `[SCHED] OK: scheduler started, 1 tasks` and idles. See `ARM64.md` at the repo root for the full port plan and §14-§15 for the setup/first-boot/purge history.

**Userland: NONE.** All pre-rebase user blobs have been purged. `proc_spawn_init()` is a no-op on aarch64 until a real aarch64-musl toolchain is wired up (phase A4). Never embed generated blob `.c` files in `kernel/arch/arm64/` — they are `.gitignore`'d, and if they reappear they lie about correctness (a 685-commit-old `/bin/sh` blob ran successfully under current syscall dispatch and masked every ARM64 regression).

**Completed (2026-04-15):** Linux arm64 Image format + `tools/gen-arm64-image.py` (A5), GICv3 driver `gic_v3.c` (B1a), DTB-driven UART probe, Rust `kernel/cap/` `aarch64-unknown-none` target added, boot oracle `tests/tests/boot_oracle_arm64.rs`. Pi 5 support deferred indefinitely.

**Remaining:** aarch64-musl userland (A4 — main blocker), PAN, TLB shootdown for SMP, `vmm_free_user_pages`.

### Lock Ordering (Canonical)

```
sched_lock > (all others)
sched_lock > waitq_lock (waitq_wake_all calls sched_wake outside its own lock)
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
