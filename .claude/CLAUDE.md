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
make INIT=vigil iso
```

**Why every step matters:**
1. `git clean -fdx` — removes ALL untracked files. `make clean` only removes `build/`. Stale `.o` files, generated `*_bin.c` files, and old binaries in `user/*/` survive `make clean`.
2. `rm -f user/vigil/vigil ...` — these are GIT-TRACKED binaries. `git clean` doesn't touch them. `make clean` doesn't touch them. They MUST be explicitly deleted or they persist across checkout/build cycles.
3. `make INIT=vigil iso` — the default `INIT` is `oksh`, NOT `vigil`. Without `INIT=vigil`, the ISO boots into oksh, not the vigil init system. Plain `make iso` embeds the WRONG init binary.

**The stale binary catastrophe (2026-03-28):** We spent an entire day debugging phantom panics caused by stale `user/vigil/vigil` (a git-tracked PIE binary from a different build environment) surviving every `make clean` and `git checkout`. Every "fix" we tested was running the same broken binary. Five AI agents, dozens of ISOs, hours of bisecting — all because `make clean` doesn't clean user binaries and `INIT=vigil` was never passed.

**NEVER trust `make clean` alone. NEVER omit `INIT=vigil`. ALWAYS nuke user binaries.**

**NEVER use incremental builds for ISO creation.** Even `make -C user/lumen && make INIT=vigil iso` can produce a stale ISO — the rootfs.img is only rebuilt when Make thinks its dependencies changed, but timestamp-based dependency tracking misses cases where binaries were rebuilt in-place. The Makefile now forces rootfs deletion before rebuild, but always use the full nuclear sequence above.

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
kernel/arch/x86_64/   ← ALL x86-specific code. Nowhere else.
kernel/core/          ← Architecture-agnostic logic only. Must port to ARM64 tomorrow.
kernel/mm/            ← Memory management (arch-agnostic)
kernel/cap/           ← Capability subsystem (Rust)
kernel/fs/            ← VFS and filesystem drivers
kernel/drivers/       ← Hardware drivers (NVMe, xHCI, virtio-net, RTL8125, etc.)
kernel/net/           ← Network stack (netdev_t, Ethernet, ARP, IP, TCP, UDP, ICMP)
kernel/sched/         ← Scheduler
tests/                ← Test harness and expected output
tools/                ← Build helpers, QEMU wrappers
docs/                 ← Architecture docs, capability model spec
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

QEMU must boot to init prompt in under 30 seconds. If it takes longer, something is wrong — investigate before running tests. `BOOT_TIMEOUT=900` in test scripts is a CI safety net, not a target. Normal boots are 10–20 seconds.

Debug cycle: **one failure → diagnose → fix → one retry**. Do NOT retry the same failing test more than twice without changing something.

---

## Debug Tooling

CFLAGS includes `-g` and `-fno-omit-frame-pointer` for reliable stack unwinding.

**ASLR design constraint:** When ASLR is eventually implemented, debug builds MUST disable it so that `make sym ADDR=...` continues to resolve addresses deterministically. Panic backtraces are useless if load addresses are randomized and not logged.

**Panic backtrace:** `isr_dispatch` calls `panic_backtrace(s->rbp)` on kernel-mode exceptions (CS=0x08). Prints up to 16 return addresses. Ring-3 faults (CS=0x23) skip backtrace.

```
[PANIC] backtrace (resolve: make sym ADDR=0x<addr>):
[PANIC]   [0] 0xffffffff80107abc
```

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
| VGA text mode | ✅ | 80×25, ROM font |
| printk | ✅ | serial-first, VGA-conditional; `%u` for unsigned, no `%d` |
| Test harness | ✅ | GRUB ISO + ANSI-strip + CRLF-strip + diff |
| PMM | ✅ | Bitmap allocator; 4KB pages; no contiguous multi-page |
| VMM + paging | ✅ | Higher-half at 0xFFFFFFFF80000000; identity map removed |
| KVA allocator | ✅ | Bump allocator; `kva_free_pages` unmaps but VA not reclaimed |
| SMAP + uaccess | ✅ | CR4.SMAP; `copy_from_user`/`copy_to_user`; EFAULT on bad ptr |
| IDT | ✅ | 48 vectors; PIC EOI before handlers |
| PIC | ✅ | 8259A remapped; IRQ0-15 → vectors 0x20-0x2F |
| PIT | ✅ | 100 Hz; `arch_get_ticks()`; runs: sched, xhci, netdev_poll_all, tcp_tick |
| PS/2 keyboard | ✅ | Scancode→ASCII; ring buffer; `kbd_read()` + `kbd_poll()` |
| Scheduler | ✅ | Preemptive round-robin; single-core; `ctx_switch` in NASM |
| GDT + TSS | ✅ | Ring-3 descriptors; RSP0 wired to kernel stack top |
| SYSCALL/SYSRET | ✅ | IA32_STAR/LSTAR/SFMASK; rdi/rsi/rdx preserved across dispatch |
| SMEP + SMEP | ✅ | CR4.SMEP + CR4.SMAP; graceful WARN if CPU lacks support |
| ELF loader | ✅ | PT_LOAD segments → user PML4 via `vmm_map_user_page` |
| User process | ✅ | Per-process PML4; per-process kernel stack; `proc_enter_user` iretq |
| Syscall dispatch | ✅ | `sys_write/read/open/close/exit/brk/mmap/fork/execve/waitpid/…` |
| Capability system | ✅ | Rust `CapSlot`; `cap_grant`/`cap_check` FFI; per-process caps[] |
| VFS + initrd | ✅ | vfs.h; initrd (18 files); console/kbd/pipe VFS devices |
| Shell + musl | ✅ | musl-gcc static binary; fork/execve/waitpid; interactive shell |
| Pipes + redirection | ✅ | `sys_pipe2`/`dup`/`dup2`; ring buffer; `\|`, `<`, `>`, `2>&1` |
| Signals | ✅ | sigaction/sigprocmask/sigreturn/kill/setfg; Ctrl-C; iretq+sysret paths |
| stat + getdents64 | ✅ | sys_stat/fstat/lstat/access/nanosleep; wc/grep/sort binaries |
| PCIe + ACPI | ✅ | MCFG+MADT on q35; ECAM ≤8 buses; graceful skip on -machine pc |
| NVMe driver | ✅ | NVMe 1.4; blkdev abstraction; synchronous poll; NSID=1 only |
| ext2 filesystem | ✅ | Read-write on nvme0p1; 16-slot LRU block cache; create/unlink/mkdir/rename; test_ext2_persistence PASS |
| xHCI + USB HID | ✅ | xHCI on q35+qemu-xhci; HID boot protocol; injects into PS/2 ring |
| Security audit | ✅ | SMEP; sa_handler validation; lseek overflow guards; O_CLOEXEC; SIGPIPE; Rust CAP bounds |
| GPT partitions | ✅ | CRC32 + primary header; nvme0p1/nvme0p2 registered; ext2 on nvme0p1 |
| virtio-net + netdev_t | ✅ | netdev_t registry; virtio-net eth0; VIRTIO_F_VERSION_1; 256-slot RX; test_virtio_net.py PASS |
| Net stack (Phase 25) | ✅ | Ethernet/ARP/IPv4/ICMP/TCP/UDP; arp_resolve uses sti+hlt; 12-byte virtio header; test_net_stack.py PASS |
| Vigil init system | ✅ | INIT=vigil; service supervision + respawn; exec_caps; test_vigil.py PASS |
| Socket API (Phase 26) | ✅ | socket/bind/listen/accept/connect/send/recv/epoll; sock_t VFS fds; httpd; test_socket.py PASS |
| writable /etc + /root | ✅ | multi-instance ramfs_t; initrd_iter_etc populates etc ramfs; /root ramfs empty at boot |
| DHCP daemon (Phase 27) | ✅ | userspace RFC 2131 client; vigil service; DISCOVER→OFFER→REQUEST→ACK; test_vigil.py PASS |
| ext2 execve | ✅ | sys_execve VFS fallback: initrd miss → vfs_open → kva_alloc → ext2_read → elf_load |
| ext2 double indirect | ✅ | i_block[13]; covers files up to ~67 MB; required for curl (1.3 MB) |
| ext2 stat (inode mode) | ✅ | vfs.c stat paths now read actual inode.i_mode from disk; fixes 0644→0775 for /bin/ |
| BearSSL + curl build | ✅ | tools/fetch-bearssl.sh + tools/build-curl.sh; curl 8.x statically linked; ext2-only via make disk |
| curl HTTPS e2e | 🔶 | Inbound TCP works (test_socket PASS). Outbound TCP connect from guest broken — see Phase 40b curl notes below |
| Thread support (Phase 29) | ✅ | clone(CLONE_VM); per-thread TLS; fd_table_t refcount; futex WAIT/WAKE; tgid; clear_child_tid; test_threads.py PASS |
| mprotect + mmap freelist (Phase 30) | ✅ | Real mprotect (W^X via NX/EFER.NXE); 64-slot VA freelist (best-fit + coalescing); test_mmap.py PASS |
| /proc filesystem (Phase 31) | ✅ | VMA tracking (170-slot kva table); procfs VFS; /proc/self/maps,status,stat,exe,cmdline,fd; /proc/meminfo; CAP_KIND_PROC_READ; test_proc.py PASS |
| TTY/PTY layer (Phase 32) | ✅ | Shared tty_t line discipline; 16 PTY pairs (/dev/ptmx + /dev/pts/N); sessions (sid); setpgid relaxed; SIGTTIN/SIGTTOU; SIGHUP on session exit; test_pty.py PASS |
| Dynamic linking (Phase 33) | ✅ | PT_INTERP+ET_DYN; MAP_FIXED+file-backed mmap; musl libc.so; vigil+login static; test_dynlink.py PASS |
| Writable root (Phase 34) | ✅ | ext2-first VFS; rootfs.img in ISO as multiboot2 module; RAM blkdev; Aegis GPT GUIDs; 0-1GB identity map; test_integrated 15/16 PASS |
| Installer (Phase 35) | ✅ | Text-mode installer; GPT+GRUB+rootfs copy; sys_blkdev_io/list/gpt_rescan; CAP_KIND_DISK_ADMIN; test_installer PASS |
| UEFI boot + EFI GRUB | ✅ | OVMF-compatible; EFI System Partition; BOOTX64.EFI via grub-mkimage x86_64-efi; custom background + unicode font |
| ACPI power button | ✅ | FADT parsing; SCI interrupt handler; DSDT _S5_ bytecode scan; short-press → ext2_sync → S5 power off |
| Framebuffer access | ✅ | sys_fb_map (513) maps FB into userspace; fb_test GUI mockup with Terminus 10x20; native resolution |
| Password asterisks | ✅ | login echoes * for password input; raw termios mode |
| DHCP no-NIC exit | ✅ | Exits on zero MAC; oneshot vigil policy (no respawn spam) |
| USB HID mouse (Phase 36) | ✅ | /dev/mouse VFS; boot protocol + PS/2 mouse; xHCI device type detection; hotplug; installer crypt(); **ThinkPad Zen 2 bare-metal PASS** |
| Lumen compositor (Phase 37) | ✅ | Backbuffer composite; z-order windows; save-under cursor; PTY terminal; taskbar; polling event loop; **ThinkPad Zen 2 bare-metal PASS** (slow rendering — optimization needed) |
| SMP (Phase 38) | ✅ | LAPIC+IOAPIC; ~30 spinlocks; SWAPGS+per-CPU GS.base; AP trampoline+SIPI; LAPIC timer; per-CPU GDT/TSS; TLB shootdown; boot oracle PASS; **ThinkPad X13 Zen 2 bare-metal PASS** |
| Glyph + Lumen optimization (Phase 39) | ✅ | libglyph.a widget toolkit; dirty-rect compositor; scheduler busy-wait fixes; MMIO skip in fork; batch yield; **make test 25/25 PASS** |
| Citadel + sys_spawn (Phase 40) | ✅ | sys_spawn (514) no-fork process creation; lumen terminal via spawn; desktop icons; /bin/sh; fb_lock re-enabled; **ThinkPad Zen 2 bare-metal PASS** (gradual lag + freeze after ~60s — optimization needed) |
| Bug fixes (Phase 40b) | ✅ | ARP deadlock from ISR (test_socket root cause since Phase 26); proc_spawn PT_INTERP (q35 RIP=0x0); socket lost-wakeup race; test_socket DHCP wait; **make test + test_socket PASS** |
| Symlinks + chmod/chown (Phase 41) | ✅ | ext2 symlinks (fast+slow); ext2_open_ex path walk; chmod/chown/lchown; DAC enforcement; ln/chmod/chown/readlink tools; **boot oracle + test_symlink PASS** |
| stsh — Styx shell (Phase 42) | ✅ | CAP_DELEGATE(13)+CAP_QUERY(14); sys_cap_query(362); sys_spawn cap_mask(5th param); line editing; history(no-persist privileged); tab completion; caps/sandbox builtins; env vars; paste detection; login fallback. **make test 25/25 PASS; ThinkPad Zen 2 bare-metal PASS** |
| Quiet boot + lumen fixes (Phase 42b) | ✅ | printk_quiet; console direct output; DHCP via stderr; sys_setfg scoped to controlling TTY; tty_read SIG_IGN bypass; lumen SIGTTIN ignore; dual-ISO test harness; login backspace; **ThinkPad Zen 2 bare-metal PASS** |

### Known deviations

| Item | Spec | Actual | Reason |
|------|------|--------|--------|
| `x86_64-elf-gcc` | native cross | symlink → `x86_64-linux-gnu-gcc` 14.2 | Not in Debian repos; equivalent with `-ffreestanding -nostdlib` |
| Boot method | QEMU `-kernel` | GRUB + `-cdrom` | QEMU 10 dropped ELF64 multiboot2 via `-kernel` |
| Serial isolation | direct diff | strip ANSI + strip CRLF + `grep ^[` + diff | SeaBIOS/GRUB contaminate COM1; serial driver uses CRLF |
| QEMU test flags | `-nographic` | `-display none -vga std` | GRUB falls back to serial without a VGA device |
| Ring-3 SS selector | 0x1B (RPL=3) | 0x18 (RPL=0) on AMD | AMD 64-bit mode strips SS RPL bits; `(ss & ~3) == 0x18` check in `idt.c` accepts both |

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

## Phase 25 — Forward Constraints

**Phase 25 status: ✅ complete. `make test` passes. `test_net_stack.py` PASS.**

**Root cause of ICMP failure (fixed):** `virtio_net_hdr_t` was 10 bytes but Virtio 1.0 modern transport requires 12 bytes (includes `num_buffers` field). TX frames were 2 bytes short and RX frames 2-byte-shifted, so SLIRP misread ARP requests. Fix: `VIRTIO_NET_HDR_SIZE=12u`. `arp_resolve` now uses `sti;hlt;cli` instead of cli+busy-poll so the PIT ISR can drive RX.

### Architecture constraints

3. **UDP RX delivery is a stub.** `udp_rx()` parses the header and looks up the port binding table but returns without delivering. Delivery to socket deferred to Phase 26 (socket API).

4. **TCP has no socket API.** The 32-slot connection table, state machine, and retransmit timers are implemented. But `sys_socket`/`bind`/`listen`/`accept`/`connect`/`send`/`recv` don't exist. TCP connections can be established in-kernel but are invisible to userspace. Phase 26.

5. **ARP busy-poll disables interrupts.** `arp_resolve()` calls `cli` for up to 300,000 iterations (~100ms). This blocks preemption during ARP resolution from the syscall path. Async ARP queue is Phase 26+ work.

6. **Static IP hardcoded — RESOLVED in Phase 27.** `net_init()` no longer sets a static IP. DHCP daemon acquires 10.0.2.15/24 gw 10.0.2.2 via SLIRP. `sys_netcfg` not yet implemented.

7. **Shared static TX buffers.** `eth_send()` and `ip_send()` use file-scoped 1514-byte and 1500-byte static buffers. No concurrent sends. Sequential callers only. Phase 26 socket layer needs a per-process TX lock or ring.

8. **PCI Bus Master Enable not set.** QEMU SeaBIOS enables Bus Master for all devices at boot. On real bare-metal UEFI, must set PCI command register bits 1+2 explicitly in `virtio_net_init()` before Phase 27 (RTL8125).

9. **No interrupt-driven RX.** Polling at 100 Hz via PIT. MSI/MSI-X deferred to v2.0.

10. **`ext2_readdir()` is a stub.** Always returns -1. `getdents64` on ext2-mounted directories won't work until this is implemented.

11. **`nvme0p2` registered but unmounted.** Available as blkdev, no filesystem uses it.

12. **`sys_nanosleep` busy-waits.** Spins on PIT ticks instead of blocking the task. Should use `sched_block` with a wakeup timer.

---

## Phase Roadmap

| Phase | Content | Status |
|-------|---------|--------|
| 25 | Ethernet/ARP/IPv4/ICMP/TCP/UDP stack | ✅ Done |
| 26 | POSIX socket API + epoll | ✅ Done |
| 27 | DHCP daemon, writable /etc+/root, BearSSL+curl, CSPRNG | ✅ Done |
| 28 | **curl HTTPS e2e** — DNS resolution, TCP outbound, TLS 1.2 handshake, HTTP/1.1 response via SLIRP NAT | ✅ Done |
| 29 | **Threads** — `clone()`+`futex`; per-thread TLS; shared address space; `CAP_KIND_THREAD_CREATE` gate; musl pthreads support | ✅ Done |
| 30 | **mprotect + mmap improvements** — real mprotect (W^X via NX); munmap VA freelist (64-slot best-fit + coalescing) | ✅ Done |
| 31 | **/proc filesystem** — capability-gated virtual FS; /proc/self/maps, /proc/self/exe, /proc/meminfo; `CAP_KIND_PROC_READ` | ✅ Done |
| 32 | **TTY/PTY layer** — proper termios; pseudo-terminals; job control (tcsetpgrp/SIGTSTP/SIGCONT); session leaders | ✅ Done |
| 33 | **Dynamic linking** — ELF interpreter (musl ldso); PT_INTERP+ET_DYN; MAP_FIXED+file-backed mmap; most binaries dynamic | ✅ Done |
| 34 | **Writable root** — ext2-first VFS; embedded rootfs.img in ISO as multiboot2 module; RAM blkdev for live boot; Aegis GPT GUID family; NVMe-fail warning | ✅ Done |
| 35 | **Installer** — text-mode; partition NVMe with Aegis GUIDs, flash rootfs.img to NVMe, install EFI GRUB | ✅ Done |
| 35b | **Bare-metal polish** — UEFI EFI boot, ACPI power button, gfxmode=auto, GRUB background, password asterisks | ✅ Done |
| 36 | **USB HID mouse** — boot protocol + PS/2 mouse; /dev/mouse VFS; installer crypt() | ✅ Done |
| 37 | **Lumen** — display compositor; backbuffer composite, z-order windows, PTY terminal, save-under cursor, taskbar | ✅ Done |
| 38 | **SMP** — LAPIC+IOAPIC, ~30 spinlocks, SWAPGS, per-CPU GS.base, AP trampoline, LAPIC timer, TLB shootdown | ✅ Done |
| 39 | **Glyph** — widget toolkit (libglyph.a); dirty-rect compositor; PTY terminal fix | 🔶 PTY hang |
| 40 | **Citadel** — sys_spawn syscall; lumen terminal via spawn (no fork); desktop icons; /bin/sh shell | ✅ Done |
| 41 | **Symlinks + chmod/chown** — ext2 symlinks; POSIX DAC permission enforcement; chmod/chown/lchown syscalls | ✅ Done |
| 42 | **stsh** — the Styx shell. Capability-aware control plane. `caps`/`sandbox` builtins; `CAP_KIND_CAP_DELEGATE` + `CAP_KIND_CAP_QUERY` syscalls; line editing; history; tab completion; paste detection. Login chain grants delegate/query caps; exec baseline does not. | ✅ Done |
| 42b | **Quiet boot + lumen fixes** — printk_quiet, console output routing, SIGTTIN bypass for compositor, dual-ISO test harness, login backspace | ✅ Done |
| 43a | **Deep architecture audit** — file-by-file review of kernel + userspace. Prioritized fix list. Multiple parallel agents. | ✅ Done |
| 44 | **IPC** — Unix domain sockets (SO_PEERCRED); POSIX shared memory (MAP_SHARED); fd passing; all capability-gated. **Required for**: Glyph external apps (pixel buffers), capability helper daemons (authenticated grant channel). Until Phase 44, all GUI apps are compiled into Lumen. | Not started |
| 45 | **capd + capability helpers** — `sys_cap_grant` (runtime delegation); `capd` daemon (declarative policy files, Unix socket, audit log); stsh helper management; stable broker interface (replaceable). Requires IPC (Phase 44) for kernel-attested peer credentials. | Not started |
| 46 | **Timers** — setitimer/alarm/timerfd; POSIX interval timers; nanosleep via sched_block (replace busy-wait) | Not started |
| 47 | **Bastion** — graphical display manager (login screen); replaces text login | Not started |
| 48 | **GUI installer** — graphical version of text-mode installer using Glyph; partition management UI; progress display | Not started |
| 49 | **Deep security audit** — adversarial review of every attack surface. Syscall fuzzing, capability bypass, TOCTOU races, integer overflows, use-after-free, signal reentrancy, ext2 corruption resilience, VFS traversal, kernel info leaks, SMP atomicity. CVE-grade findings block release. pwn binary expanded. | Not started |
| 50 | **Release** | Not started |
| — | RTL8125 2.5GbE driver (PCI 10ec:8125) — post-release, requires WiFi confirmed working | Not started |

---

## Aegis GPT Partition Type GUIDs — Installer Reference

**This section is permanent project documentation. Do not condense, summarize, or remove during cleanup.**

Aegis uses a custom GPT partition type GUID family to identify its own disk partitions. This prevents the kernel from accidentally mounting an incompatible ext2 (or other) volume — a critical safety property since the fallback is volatile RAM storage.

### GUID Structure

Every Aegis partition type GUID shares a fixed 8-byte prefix. The remaining 8 bytes encode the partition's role:

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

Role `0001` (Root) is the only one implemented. The others are reserved for the installer and future phases.

### How It Works

1. **`make disk` / installer:** Creates GPT partitions with `sgdisk --typecode=1:A3618F24-0C76-4B3D-0001-000000000000` instead of the generic Linux `8300` type code.

2. **`gpt_scan()` in kernel:** When scanning GPT entries, compares each partition's type GUID against the 8-byte Aegis prefix. Partitions that don't match are silently skipped — they are never registered as blkdevs and are invisible to the rest of the kernel.

3. **`ext2_mount("nvme0p1")`:** Only succeeds if `nvme0p1` was registered by `gpt_scan`, which only happens for Aegis-prefixed partitions.

4. **Safety guarantee:** If an NVMe drive contains a non-Aegis ext2 partition (e.g., a Linux install, a different OS), the kernel will not mount it. It falls back to the ramdisk (live ISO boot) and prints a warning if NVMe hardware was detected but no Aegis root partition was found.

### Installer Requirements (Phase 35)

The installer must:
- Create partitions using the Aegis GUID family (not generic `8300`)
- Write the role bytes correctly (root = `0001`, swap = `0002`)
- Flash `rootfs.img` (the ext2 image from the ISO module) to the root partition
- The kernel on next boot will find the Aegis-typed partition via `gpt_scan` and mount it from NVMe

### Why Not Just Check the ext2 Volume Label?

Volume labels (`mke2fs -L aegis-root`) are mutable and checked after reading the superblock. The GPT type GUID is checked during partition table scan — before any filesystem code runs. A wrong GUID means the partition is never even opened. This is a stronger guarantee with a smaller attack surface.

---

## Aegis GUI Architecture — Lumen/Glyph/Citadel

**This section is permanent project documentation. Do not condense, summarize, or remove during cleanup.**

Aegis has its own native GUI stack — no X11, no Wayland, no bloat. The framebuffer is mapped directly into userspace via `sys_fb_map` (syscall 513). The GUI stack is built from scratch with clean, simple components.

### Components

| Codename | Role | Linux Equivalent | Description |
|----------|------|-----------------|-------------|
| **Lumen** | Display server + compositor | Wayland compositor | Owns the framebuffer. Composites client windows. Dispatches keyboard/mouse events to the focused window. Renders the cursor. Single process, no network transparency. |
| **Glyph** | Widget toolkit library | GTK/Qt | `libglyph.so` — shared library that apps link against. Provides buttons, labels, text input, window chrome, layout. Apps `#include <glyph.h>`. Dynamically linked. |
| **Citadel** | Desktop shell | GNOME Shell / KDE Plasma | Taskbar, application launcher, desktop icons, clock, system tray. First real Glyph application. |
| **Bastion** | Display manager | GDM / SDDM | Graphical login screen. Replaces text-mode login. Future work. |

### Architecture (v0.1)

In v0.1, Lumen + Citadel run as a single process that owns the framebuffer. Glyph is a shared library. The window manager is built into Lumen (no separate WM process).

```
┌─────────────────────────────────────────────┐
│                 Framebuffer                  │
│            (mapped via sys_fb_map)           │
├─────────────────────────────────────────────┤
│              Lumen (compositor)              │
│  - owns FB mapping                          │
│  - keyboard/mouse event dispatch            │
│  - window management (built-in v0.1)        │
│  - cursor rendering                         │
├──────────┬──────────┬───────────────────────┤
│ Citadel  │ App 1    │ App 2    ...          │
│ (shell)  │          │                       │
├──────────┴──────────┴───────────────────────┤
│           libglyph.so (widgets)             │
│  buttons, labels, text, windows, layout     │
└─────────────────────────────────────────────┘
```

### Font

The GUI uses **Terminus** bitmap font (SIL Open Font License 1.1):
- Terminal + GUI: `ter-u20n` (10×20) — 96 columns × 54 rows at 1080p
- Embedded as C array in userspace binaries via header file
- Same font should be used for the kernel framebuffer terminal (replace VGA 8×16)

### Input

- **Keyboard:** USB HID boot protocol via xHCI (already working)
- **Mouse:** USB HID boot protocol via xHCI (Phase 36 — 3-byte reports: buttons + X/Y deltas)
- **PS/2:** Keyboard already working; PS/2 mouse support is future work

### Design Principles

1. **No protocol overhead.** Lumen is not a network-transparent display server. It's a local compositor that draws directly to the framebuffer.
2. **One toolkit.** Glyph is THE widget library. No competing toolkits, no abstraction layers.
3. **Dynamically linked.** `libglyph.so` is a shared library. Apps link against it at build time and load it at runtime via the dynamic linker.
4. **Developer-friendly.** `#include <glyph.h>`, link with `-lglyph`, get windows and widgets. Simple C API.
5. **Capability-gated.** Framebuffer access requires appropriate capabilities. Random processes cannot draw to the screen.

---

**RTL8125 testing note:** The machine has RTL8125B (ASUS, PCI 0a:00.0, IOMMU group 18) managed by host `r8169`. WiFi is MT7921K (0b:00.0). Do NOT test RTL8125 until WiFi is confirmed working — you will lose remote access.

**Test hardware:**
- **Dev machine (this one):** ASUS with Ryzen 7 6800H, RTL8125B (10ec:8125, PCI 0a:00.0), Intel Wi-Fi MT7921K
- **Test machine A:** Ryzen 7 6800H machine (TBD), dual RTL8168 (10ec:8168, PCI 02:00.0 and 03:00.0), Intel AX200 Wi-Fi, Samsung NVMe (144d:a80c, PCI 01:00.0) — for RTL8168 driver testing
- **Test machine B (ThinkPad X13 Gen 1):** Ryzen 7 Pro 4750U (Zen 2 / Renoir)
- **Shared panic (both test machines A and B) — FIXED 2026-03-24:** `[PANIC] corrupt ring-3 iretq frame vec=32 ss=0x18 rsp=0x7ffffffe7d0` after `[SHELL] Aegis shell ready`. Root cause: AMD CPUs in 64-bit long mode strip RPL bits from SS (pushing 0x18 instead of 0x1B on interrupt entry) since SS is unused for addressing in 64-bit mode. Two-part fix: (1) relaxed panic check from `ss != 0x1B` to `(ss & ~3) != 0x18`; (2) added SS RPL normalization in `isr_dispatch` — `if (s->cs == 0x23) s->ss |= 3;` forces RPL=3 before iretq so AMD machines don't #GP. **AMD 64-bit behavior: SS RPL bits are not maintained; expect ss=0x18 on interrupt from ring-3 on AMD, ss=0x1B on Intel/QEMU.**

---

## Phase 35b — Forward Constraints

**Phase 35b status: ✅ complete. Bare-metal working on ThinkPad X13.**

1. **UEFI only for installed boot.** No BIOS/CSM boot from NVMe (SeaBIOS lacks NVMe INT 13h). Live ISO still boots via legacy BIOS (grub-mkrescue handles both).

2. **ACPI power button has no `_PTS`/`_GTS` calls.** No AML interpreter — we skip firmware preparation methods before S5. Works on QEMU and ThinkPad. Some hardware may leave EC in a bad state.

3. **`_S5_` SLP_TYPa found by DSDT bytecode scan.** Not a full AML parser — scans for the literal "_S5_" string. Works on all tested platforms. Exotic DSDT layouts could fail.

4. **GRUB background only on installed system.** The live ISO uses grub-mkrescue's default theme. Installed ESP has custom bg.jpg + light-green font.

5. **fb_test owns framebuffer exclusively during display.** Other processes' printk output can still overwrite it. A proper solution requires Lumen (compositor) to own the FB exclusively and suppress kernel FB writes.

6. **sys_fb_map maps FB physical pages into user VA.** On process exit, pmm_free_page silently skips MMIO addresses. The mapping is not revoked — a second call to sys_fb_map creates a duplicate mapping.

7. **DHCP is oneshot.** On systems WITH a NIC, DHCP acquires a lease and stays running for renewal. If the daemon crashes, vigil does NOT restart it. Manual restart: `dhcp &`.

8. **AUTH capability in execve baseline.** Every exec'd binary can open /etc/shadow. Phase 40 security audit should restrict to login + installer only.

---

## Phase 36 — Forward Constraints

**Phase 36 status: 🔶 Code complete, untested. Boot oracle hangs on x86 box (VNET RX noise in serial output).**

**What was implemented:**
- USB HID mouse driver (`kernel/drivers/usb_mouse.c`): 128-entry ring buffer, 3-byte boot protocol parser, blocking read via sched_block/sched_wake
- xHCI device type detection: GET_DESCRIPTOR(Configuration) during enumeration, bInterfaceProtocol check (1=kbd, 2=mouse), SET_PROTOCOL(Boot)
- USB hotplug: Port Status Change TRB handler in xhci_poll(), per-slot port tracking, silent disconnect
- `/dev/mouse` VFS device: registered in initrd.c, stat in vfs.c, reads mouse_event_t structs (7 bytes packed)
- mouse_test binary + test_mouse.py: QEMU q35 with usb-mouse, monitor event injection
- Installer password hashing: crypt() with SHA-512 ($6$), /dev/urandom salt, termios asterisk echo, -lcrypt
- boot.txt cap count fixed: 10→9 (was stale since Phase 35b)

**Known issues:**
1. **q35 without NVMe panics at RIP=0x0.** Pre-existing bug (reproduces on pre-Phase36 baseline). On `-machine q35` without an NVMe drive attached, init crashes with `RIP=0x0, fs_base=0x0, CS=0x23`. Adding `-drive ... -device nvme,...` makes it work. This is suspicious — the live system boots from ramdisk, not NVMe. The NVMe presence shouldn't affect init loading. Root cause unknown. Likely: PCIe enumeration without NVMe corrupts ELF loader state or an uninitialized variable. **Needs `make gdb` investigation** with breakpoints in `elf_load` and `proc_enter_user` on q35 to see why entry point is 0. Test scripts currently paper over this by adding NVMe+virtio-net to match test_integrated's working config.

2. **`[PCIE] found %04x:%04x` — printk format bug.** `pcie.c:187` uses `%d` and `%04x` but printk only supports `%u`, `%x`, `%s`, `%c`. The `%d` and `%04x` come out literally. Cosmetic only — fix by changing to `%u`/`%x`.

2. **No cursor rendering.** Kernel delivers raw deltas only. Cursor position tracking and rendering are Lumen's job (Phase 37).

3. **Single mouse only.** First boot-protocol mouse found is used. Additional mice ignored.

4. **No scroll wheel.** Boot protocol is 3 bytes (buttons + dx + dy). Extended HID reports with scroll require report descriptor parsing.

5. **No PS/2 mouse.** Only USB HID via xHCI.

6. **Hotplug has no debounce.** Rapid connect/disconnect could flood the event ring.

7. **`crypt()` is slow.** SHA-512 ~50ms on real hardware. Fine for installer, not for high-frequency auth.

8. **Control transfer helper reuses s_hid_buf.** The GET_DESCRIPTOR response goes into the same 4KB page later used for interrupt IN reports. Safe because detection happens before first interrupt IN is scheduled.

---

## Phase 37 — Forward Constraints

**Phase 37 status: 🔶 Code complete, untested. Builds clean on x86 box.**

**What was implemented:**
- Lumen compositor (`user/lumen/`): 6 source files, ~1200 lines total
- Drawing library (draw.c): surface_t-based px, fill_rect, rect, gradient, char, text, blit
- Software cursor (cursor.c): 16x16 monochrome arrow, save-under buffer
- Compositor (compositor.c): z-ordered window list (16 max), click-to-focus, titlebar drag, close button, window chrome with gradient title bars, backbuffer composite with memcpy flip
- Terminal (terminal.c): PTY-based, forks oksh, text grid with scroll, raw char processing
- Taskbar (taskbar.c): Aegis button, static clock
- Main (main.c): sys_fb_map, raw TTY mode (ISIG|ICANON|ECHO disabled), polling event loop (kbd+mouse+pty master), 16ms nanosleep idle
- Kernel fix: /dev/mouse now non-blocking (returns -EAGAIN if empty)

**Forward constraints:**

1. **No multi-process clients.** Lumen v0.1 is monolithic. External apps cannot create windows. Phase 38 (Glyph) needs shared memory IPC (MAP_SHARED + fd passing).

2. **No ANSI escape parsing.** Terminal renders raw text only. Programs emitting ANSI codes display garbage. VT100 parser is Phase 38+ work.

3. **No window resize.** Fixed-size windows. Resize requires client notification + buffer realloc.

4. **Static clock.** No RTC syscall. Taskbar shows "12:00 AM" always.

5. **Full-frame composite.** Every redraw composites the entire backbuffer (~8MB memcpy at 1080p). Dirty-rect optimization deferred.

6. **printk conflicts.** Kernel printk still writes to framebuffer. Kernel output corrupts the display while lumen runs. Need a kernel flag to suppress FB writes when compositor is active.

7. **No compositor keyboard shortcuts.** Keyboard is raw ASCII from stdin. No Alt+Tab, no window switching via keyboard. Mouse-only interaction.

8. **Polling loop, not event-driven.** epoll doesn't support non-socket fds. The 16ms nanosleep means ~60fps max but also ~16ms input latency. Acceptable for v0.1.

9. **/dev/mouse is now always non-blocking.** mouse_read_fn returns -EAGAIN if no events. This changes behavior for ALL readers of /dev/mouse. Only lumen and mouse_test read it, both handle -EAGAIN correctly.

10. **Backbuffer allocated via malloc.** At 1080p: ~8MB. Total display memory: ~16MB (FB mapping + backbuffer). Each window also allocates a pixel buffer via calloc.

---

## Phase 27 — Forward Constraints

**Phase 27 status: ✅ Complete. DHCP + curl HTTPS working.**

**CA bundle note:** curl works with `-k` (skip cert verification). BearSSL CA bundle loading deferred (error 26 — BearSSL/musl interaction, not a kernel bug).

---

## Phase 38 — Forward Constraints

**Phase 38 status: ✅ complete. Boot oracle PASS. ThinkPad X13 Zen 2 bare-metal PASS.**

1. **IOAPIC remapped PIC vectors need IDT stubs.** `pic_disable()` remaps the 8259A to vectors 0xF0-0xFF before masking. On real hardware (laptops with EC/ACPI), a pending PIC interrupt (typically spurious IRQ7 = vector 0xF7) can be in-flight and delivered after STI. ISR stubs for 0xF0-0xFD are installed; `isr_dispatch` silently drops them. This was the root cause of the #GP (error=0x7BB) on ThinkPad X13 Zen 2.

2. **SWAPGS omitted from proc_enter_user.** On AMD Zen 2, SWAPGS before iretq in proc_enter_user causes a panic. Root cause not fully understood (may be AMD SS RPL interaction). Safe to omit: the first interrupt/syscall from user mode does SWAPGS at entry, restoring correct GS.base state.

3. **Single-core scheduling only.** APs start and enter halt loops but do not run user tasks. Per-CPU run queues and cross-core task migration are future work.

4. **Signal delivery from ISR context unprotected.** `signal_send_pid`/`signal_send_pgrp` traverse task list without lock. Protected by `sched_lock` in most paths but ISR path (kbd Ctrl-C) is unprotected. Low priority — single ISR context.

---

*Last updated: 2026-03-28 — Phases 36-38 bare-metal PASS on ThinkPad X13 Zen 2. IOAPIC #GP fixed (remapped PIC IDT stubs). CAP_KIND_FB added for framebuffer access. Lumen renders (slow — optimization future work).*

---

## Phase 39 — Forward Constraints

**Phase 39 status: ✅ Complete. make test 25/25 PASS.**

Phase 39 delivered Glyph widget toolkit, dirty-rect compositor, scheduler busy-wait elimination, fork MMIO skip, kernel command line parsing, and vigil boot modes. The embedded terminal (fork inside compositor) was abandoned due to bare-metal performance — resolved in Phase 40 with sys_spawn.

---

## Phase 40 — Forward Constraints

**Phase 40 status: ✅ Complete. ThinkPad Zen 2 bare-metal PASS (2026-03-29).** Lumen renders immediately, PTY terminal works (shell prompt, typing, output), desktop icons work, multi-terminal via click. Gradual lag + freeze after ~60s — likely dirty-rect accumulation or memory leak in terminal rendering. Optimization deferred.

**What was implemented:**

1. **sys_spawn syscall (514)** — creates a process from an ELF path WITHOUT fork. Fresh PML4, direct ELF load, no page copy, no vmm_window_lock contention. Handles PT_INTERP (dynamic linking), SysV ABI stack with full auxv, capabilities (baseline + exec_caps). stdio_fd parameter for PTY slave remapping. Child starts in new session.

2. **Lumen terminal rewrite** — `terminal_create` uses sys_spawn instead of fork+exec. Opens PTY master+slave, calls `sys_spawn("/bin/sh", ..., slave_fd)`. No pre-fork allocation gymnastics needed. Uses /bin/sh (not oksh).

3. **Desktop icons** — Terminal and System icons drawn via compositor `on_draw_desktop` callback. Click Terminal icon → spawn new terminal window via sys_spawn. Multi-terminal support via `glyph_window_t.tag` field (stores PTY master fd).

4. **fb_lock_compositor re-enabled** in sys_fb_map.

**Forward constraints:**

1. **sys_spawn child has no controlling terminal.** setsid is implicit (sid=pid) but TIOCSCTTY is not called. Job control (Ctrl-C, SIGTSTP) won't work in spawned terminals. The shell works for basic I/O.

2. **Desktop icons are simple clickable rectangles.** No drag, no right-click, no icon rearrangement.

3. **No multi-process GUI clients.** Phase 42 IPC required for external Glyph apps.

4. **No ANSI escape parsing.** Terminal renders raw text only. VT100 parser is future work.

5. **Lumen GUI freezes when running non-builtin commands.** On bare-metal ThinkPad, typing a non-builtin command (e.g., `ls`) in the lumen terminal causes the entire compositor to freeze. Shell builtins (echo, cd) work fine. Likely the same class of bug as the early text-mode freeze: fork+exec in the child process or waitpid in the parent blocks something in the PTY/scheduler chain that starves the compositor's event loop. Text-mode fixed this with specific PTY/sched_block fixes — same pattern needs to be applied to the lumen PTY path. **CRITICAL BUG for GUI usability.**

---

## Phase 40b — Bug Fixes (2026-03-29)

**All fixes verified: make test 25/25 PASS, test_socket PASS (first time since Phase 26).**

### ARP deadlock from ISR context (root cause of test_socket since Phase 26)

**Root cause:** When `tcp_rx` receives a SYN and sends SYN-ACK, `ip_send` → `arp_resolve` blocks with `arch_wait_for_irq` while holding `netdev_lock` + `tcp_lock` (called from PIT ISR via virtio_net_poll). The next PIT tick tries to acquire those locks → permanent deadlock. No more packets ever processed.

**Why gateway ARP was uncached:** DHCP uses broadcasts exclusively — no ARP entry created for gateway (10.0.2.2). First unicast sender (chronos NTP) resolves it, but if TCP SYN arrives first → deadlock.

**Fix (kernel/net/eth.c + kernel/syscall/sys_socket.c):**
1. `arp_resolve` detects ISR context (IF flag check) and returns -1 instead of blocking. TCP retransmit timer re-sends SYN-ACK on next tick when ARP is cached.
2. `sys_netcfg` (op=0, called by DHCP) proactively resolves gateway ARP from safe syscall context.

### proc_spawn missing PT_INTERP (root cause of q35-without-NVMe RIP=0x0)

**Root cause:** `proc_spawn` loaded the main ELF but never checked `er.interp` for a dynamic linker. Dynamically-linked init binaries (e.g., INIT=oksh) jumped through unresolved PLT stubs to address 0x0.

**Fix (kernel/proc/proc.c):** Added interpreter loading mirroring `sys_execve`'s pattern. Also added full auxv (AT_PHDR, AT_PHNUM, AT_PAGESZ, AT_ENTRY, AT_BASE, AT_PHENT) and fixed stack layout (old layout overlapped auxv with string data).

### Socket lost-wakeup race in accept/recv/connect

**Root cause:** `waiter_task` was set AFTER checking the data queue. If `sock_wake` fired between the empty-queue check and `sched_block` while `waiter_task` was still NULL, the wakeup was silently lost → `accept()` blocked forever.

**Fix (kernel/syscall/sys_socket.c):** Set `waiter_task` BEFORE checking data availability in `sys_accept`, `sys_recvfrom` (TCP + UDP), and `sys_connect`. If wake fires early, it sets RUNNING, so `sched_block` becomes a no-op.

### test_curl / outbound TCP — STILL BROKEN (investigated 2026-03-29)

**Symptom:** `curl -sk https://example.com` exits instantly with rc=7 (CURLE_COULDNT_CONNECT). No TLS handshake attempted. `curl -sv http://10.0.2.2` (SLIRP gateway, inbound-style via local subnet) works perfectly. test_socket (inbound TCP via SLIRP hostfwd) PASSES. Only outbound TCP to external IPs through SLIRP NAT is broken.

**What was diagnosed:**
- DNS resolution works (/etc/hosts has 104.18.27.120 for example.com, confirmed valid)
- Host machine can reach example.com:443 via SLIRP from host curl
- Guest `connect()` returns ECONNREFUSED (-111) immediately (not timeout)
- Only 3 VNET RX packets after curl command (SYN-ACK exchange but connection fails)
- `sys_connect` → `tcp_connect` → `tcp_send_segment` under `tcp_lock` was the original deadlock

**What was tried and failed:**
1. **IF-flag check in arp_resolve** — detected ISR context but was too aggressive: returned -1 even from syscall context where IRQs are disabled due to spinlocks. Broke ALL outbound TCP including HTTP to SLIRP gateway. Replaced with `g_in_netdev_poll` flag.
2. **g_in_netdev_poll flag** — correctly identifies PIT ISR RX path only. Fixed test_socket (inbound accept works). But outbound connect still broken because...
3. **tcp_connect release tcp_lock before SYN send** — moved `tcp_send_segment` after `spin_unlock_irqrestore` so arp_resolve can block safely. But tcp_tick could fire between unlock and send, seeing the SYN_SENT connection and potentially resetting it.
4. **Far-future retransmit_at** — set `retransmit_at = ticks + RTO + 200` before unlock, real deadline after send. Didn't help — curl still exits instantly with rc=7.

**Remaining theories:**
- `tcp_send_segment` outside `tcp_lock` races with `tcp_tick` modifying the same `tcp_conn_t`. The connection may be getting reset by tcp_tick before the SYN-ACK arrives.
- The SYN may be going out but SLIRP's SYN-ACK arrives during a window where the connection state is inconsistent.
- musl's `getaddrinfo` returns a second garbage address `0.0.0.2` alongside the /etc/hosts entry — curl tries both, both fail, reports the last error.
- The 3 VNET RX packets may be ARP reply + TCP RST rather than SYN-ACK.

**What works:** Inbound TCP (SLIRP hostfwd → guest httpd), HTTP to SLIRP gateway (10.0.2.2), DHCP (UDP broadcast), ARP resolution. The TCP stack fundamentally works for server-side; the outbound connect path has a race condition or state corruption issue.

**Next steps:** Add printk-level TCP debug logging (SYN sent, SYN-ACK received, RST received, state transitions) to see exactly what happens during outbound connect. Or use `make gdb` with breakpoints in tcp_connect/tcp_rx to trace the handshake.

---

*Last updated: 2026-03-29 — Phase 41 implementation complete, awaiting bare-metal test.*

---

## Phase 41 — Forward Constraints

**Phase 41 status: ✅ Complete. Boot oracle PASS. test_symlink.py PASS.**

1. **No symlinks on ramfs/initrd.** Only ext2 supports symlinks. `/tmp`, `/run`, `/dev`, and initrd files cannot be symlink targets or sources within their own namespace.

2. **No hard links.** `link()` / `linkat()` not implemented. Only symlinks.

3. **Symlink depth limit is 8.** Returns `-ELOOP` beyond 8 levels. Sufficient for Aegis.

4. **Permission enforcement only on ext2.** ramfs, initrd, procfs, and device files are not permission-checked. They are kernel-internal resources gated by capabilities.

5. **No uid=0 bypass.** Root (uid=0) is subject to the same DAC checks as any other user. This is by design — Aegis has no ambient authority.

6. **`ext2_open_ex` uses 512-byte `resolved` buffer.** Paths longer than 511 bytes after symlink expansion are truncated.

7. **umask not applied to ext2_create.** `ext2_create` is called with hardcoded 0644. The process umask should be consulted.

8. **Boot-time file opens bypass DAC.** `vfs_open` checks `sched_current()->is_user` — kernel-context opens (during init, before user tasks run) skip permission checks. This is correct: kernel opens during boot are trusted.

---

## Phase 42 — Forward Constraints

**Phase 42 status: 🔶 Code complete. Awaiting build box test.**

1. **Grant builtin deferred to Phase 45 (capd).** stsh can query caps and restrict caps on spawn, but cannot grant capabilities to running processes. Requires capd (IPC-based capability broker).

2. **No scripting.** No if/for/while/case/functions. stsh v1 is interactive only.

3. **No command substitution.** `$(...)` and backticks not supported.

4. **No glob expansion.** `*.c` passed literally to commands.

5. **No job control.** No bg/fg/Ctrl-Z. Single foreground pipeline only.

6. **Hostname hardcoded to "aegis".** No gethostname syscall.

7. **Tab completion not capability-aware (v1).** Shows all entries regardless of permissions. Capability-aware filtering is v2.

8. **sys_spawn has 5 parameters.** All existing callers updated to pass 0/NULL for cap_mask.

9. **stsh is ext2-only.** Login falls back to /bin/sh if stsh not found (no NVMe on -machine pc).

10. **Privileged history is in-memory only.** Sessions with CAP_DELEGATE never write ~/.stsh_history. Prevents attacker from mining operator commands from disk.

---

## Architecture Audit (2026-03-29) — 8-Agent Deep Review

Eight parallel agents audited the kernel against Unix best practices, Linux/xv6 patterns, and ARM64 port status. Results organized by severity.

### CRITICAL — Fix Before Release

| ID | Issue | File | Status |
|----|-------|------|--------|
| X1 | **SYSRET non-canonical RIP via sigreturn** | sys_signal.c | ✅ Fixed Phase 39 |
| X2 | **Sigreturn RFLAGS sanitization** | sys_signal.c | ✅ Fixed Phase 39 |
| X3 | **execve point-of-no-return** | sys_process.c | ✅ Fixed Phase 39 |
| X4 | **alloc_table OOM returns error** | vmm.c | ✅ Fixed Phase 39 |

### HIGH — Fix Soon

| ID | Issue | File | Status |
|----|-------|------|--------|
| C2 | **Orphan reparenting to init** | sys_process.c | ✅ Fixed Phase 39 |
| C5 | **SYSRET signal delivery saves incomplete registers** — only r8-r10, rip, rflags, rsp saved. Callee-saved registers (rbx, rbp, r12-r15) corrupted after signal handler. ISR path is correct. | signal.c:360 | **TODO** |
| C3 | **sys_fork leak fix (fd_table + VMA)** | sys_process.c | ✅ Fixed Phase 39 |

### MEDIUM — Track and Fix

| ID | Issue | File | Status |
|----|-------|------|--------|
| C6 | **epoll_wait lost-wakeup** | epoll.c | ✅ Fixed Phase 39 |
| P2 | **vmm_free_user_pml4 optimization** | vmm.c | ✅ Fixed Phase 39 |
| M1 | **sched_wake has no memory barrier** — safe on x86, broken on ARM64 (store reordering). | sched.c:352 | **TODO** |
| M2 | **mmap freelist has no lock for CLONE_VM threads** — safe single-core, corruption on SMP. | sys_memory.c | **TODO** |
| M3 | **vmm_window_lock > pmm_lock ordering fragile** — non-nested today but undocumented. Future code could create ABBA deadlock. | vmm.c | Document |

### COMPLETED (from previous + this session)

- [x] sys_nanosleep busy-wait → sched_block + sleep_deadline ✅
- [x] PTY slave/master read busy-wait → sched_block + sched_wake ✅
- [x] kbd_read_interruptible busy-wait → sched_block + s_kbd_waiter ✅
- [x] Fork MMIO page skip (VMM_FLAG_WC/UC) ✅
- [x] Fork batch yield (every 32 pages) ✅
- [x] fb_lock_compositor re-enabled ✅
- [x] All prior Phase 38 audit items (printk format, lock ordering, capability gaps, etc.) ✅

### Performance Recommendations (Future)

| ID | Issue | Impact | Effort |
|----|-------|--------|--------|
| P1 | **COW fork** — mark PTEs read-only, copy-on-write fault handler. Fork drops from 30s to <1ms. Skip MMIO pages entirely. | Eliminates fork stall | Large (vmm.c, pmm.c, idt.c) |
| P3 | **Separate run queue** — RUNNING-only doubly-linked list. Blocked tasks on wait queues. sched_tick O(1). | Scheduler scalability | Medium (sched.c) |
| P4 | **draw_px optimization** — bypass bounds check for known-good rects, or use direct buffer writes for fill_rect. | Faster GUI rendering | Small (draw.c) |
| WQ | **Wait queue abstraction** — `waitq_t` with stack-allocated entries, lost-wakeup-proof add-before-unlock pattern. Replaces ad-hoc single-task waiting pointers. | Cleaner blocking I/O | Medium (new file + all blockers) |

### ARM64 Port Status

**Build: BROKEN.** xhci.c fails on GCC 15 array-bounds. 8 source files missing from ARM64 Makefile (fd_table, tty, pty, procfs, vma, sys_disk, futex, usb_mouse). `smp_percpu_init_bsp()` not called — `sched_current()` dereferences garbage.

**Minimum to fix (~2 hours):** Fix xhci.c compile, add missing .o files, add `smp_percpu_init_bsp()` call, verify QEMU virt boot.

**Remaining ARM64 work:** TTBR0/TTBR1 split (process isolation), PAN enable, vmm_free_user_pages real implementation, Rust cap cross-compile.

### Lock Ordering (Canonical, document-of-record)

```
vmm_window_lock > pmm_lock > kva_lock
sched_lock > (all others)
tcp_lock before sock_lock (deferred wake pattern)
ip_lock before arp_lock (copy-then-release pattern)
```

---

## Phase 34 — Forward Constraints

**Phase 34 status: ✅ complete. Boot oracle PASS. test_integrated 15/16 PASS (DHCP timing).**

1. **Module memory is permanently reserved.** ~60MB of physical RAM for the GRUB module is never freed. The ramdisk copies it into fresh KVA pages (~60MB more). Total: ~120MB. On installed systems (NVMe ext2), both the module and copy are wasted. Future optimization: skip ramdisk_init when NVMe ext2 mounts successfully.

2. **Ramdisk is volatile.** Writes go to RAM, lost on reboot. The installer (Phase 35) copies rootfs.img to NVMe for persistence.

3. **Only one multiboot2 module supported.** First module tag found is used.

4. **Aegis GUID prefix required for NVMe.** Partitions without `A3618F24-0C76-4B3D` prefix are invisible. Installer must create partitions with correct GUIDs.

5. **Boot oracle is fragile.** PMM line varies with RAM size (hardcoded to 2G). DHCP retry lines are filtered. `[SHELL]` line appears if boot is fast enough for vigil→login→shell within the timeout. Future: regex-based oracle.

6. **Identity map covers 0-1GB.** boot.asm pd_lo fills 512 entries. vmm_init creates a matching pd_lo. `alloc_table_early` and `ramdisk_init` (module copy) rely on this. Systems with modules above 1GB would need additional PDPT entries.

7. **Vigil service run files must start with `/`.** Vigil checks `run_cmd[0]=='/'` to decide between direct execv (preserves exec_caps) and `sh -c` (loses exec_caps). ext2 run files must not have `exec` prefix.

8. **curl double-faults with 2G RAM.** Pre-existing issue — curl's TLS stack overflows with the larger memory layout. Needs separate investigation.

9. **test_ext2 writes to /home, not /tmp.** `/tmp` is now ramfs (volatile). Persistence tests must use ext2-backed paths.

---

## Phase 35 — Forward Constraints

**Phase 35 status: ✅ complete. test_installer PASS.**

1. **No UEFI boot.** BIOS+GPT only. UEFI requires EFI System Partition + different GRUB image. SeaBIOS cannot boot NVMe directly via INT 13h — installed system needs UEFI firmware (OVMF) for true NVMe-only boot. Test uses ISO as GRUB loader with NVMe data path.

2. **Single NVMe only.** Installer targets `nvme0`. No device selection menu.

3. **No resize/dual-boot.** Installer wipes entire disk. No partition resizing, no dual-boot.

4. **GRUB prefix hardcoded.** `(hd0,gpt2)/boot/grub`. If partition layout changes, rebuild core.img.

5. **DISK_ADMIN in execve baseline.** Every exec'd binary gets `CAP_KIND_DISK_ADMIN`. Phase 38 security audit should restrict to specific binaries.

6. **No installed-system kernel update path.** Must re-run installer to update kernel.

7. **grub.cfg uses (hd0,gpt2) not UUID.** Works for single-disk. Multi-disk needs `search --fs-uuid`.

8. **Installer runs from shell, not vigil.** The user must manually invoke `/bin/installer`. It gets DISK_ADMIN via the execve baseline.

---

## Phase 33 — Forward Constraints

**Phase 33 status: ✅ complete. Boot oracle PASS. `test_integrated.py` 16/16 PASS.**

### Current filesystem layout (important for Phase 34)

The system has **three distinct data sources** that the VFS merges at runtime:

1. **Initrd** (baked into kernel image, read-only): 20 files total.
   - Static binaries: `/bin/login`, `/bin/vigil`, `/bin/sh`, `/bin/echo`, `/bin/cat`, `/bin/ls`
   - Config files: `/etc/motd`, `/etc/passwd`, `/etc/shadow`, `/etc/profile`, `/etc/hosts`
   - Vigil service descriptors: `/etc/vigil/services/{getty,httpd,dhcp}/{run,policy,caps}`
   - These are the ONLY things available before ext2 is mounted.

2. **ext2 on NVMe** (read-write disk, mounted after NVMe+GPT init): everything else.
   - Dynamic binaries: `/bin/oksh`, `/bin/grep`, `/bin/wc`, `/bin/sort`, `/bin/curl`, etc. (26 binaries)
   - Shared library: `/lib/libc.so`, `/lib/ld-musl-x86_64.so.1`
   - Auth files (duplicated from initrd): `/etc/passwd`, `/etc/shadow`, `/etc/group`
   - Vigil service configs (duplicated from initrd)
   - CA certs: `/etc/ssl/certs/ca-certificates.crt`

3. **ramfs** (writable in-memory, populated at boot): `/etc` and `/root` only.
   - `/etc` ramfs is populated from initrd's `/etc/*` files at mount time via `initrd_iter_etc`.
   - `/root` ramfs is empty at boot.
   - These are the ONLY writable paths outside ext2.

**VFS open order:** initrd first → ext2 fallback. File opens check initrd, then VFS (ext2/ramfs).
**VFS directory listing:** `/bin` and `/lib` listings come from ext2 (initrd has no synthetic `/bin` directory). `/etc` listing comes from ramfs. `/` listing comes from initrd's synthetic root (shows etc, bin, dev, lib, root).

**The merge is implicit and incomplete.** `ls /bin` only shows ext2 entries — the 6 initrd binaries (login, vigil, sh, echo, cat, ls) are invisible in directory listings but accessible by name. Phase 34 (writable root) should make this coherent: one writable tree that contains everything.

### Forward constraints

1. **No ASLR.** Interpreter loads at fixed INTERP_BASE (0x40000000). Debug builds must disable ASLR to keep `make sym` working.

2. **dlopen/dlsym untested.** Infrastructure supports it, no test exercises it.

3. **curl statically links BearSSL.** Only libc is shared.

4. **No LD_PRELOAD or LD_LIBRARY_PATH.** Interpreter uses built-in /lib path only.

5. **File-backed mmap is read-once.** No page cache, no demand paging.

6. **No MAP_SHARED.** MAP_PRIVATE file-backed mappings only.

7. **Test suite consolidated.** `test_integrated.py` runs 16 tests in one QEMU boot (replaces 11 separate boots). `make test` builds both vigil ISO and disk before running. Hardware-specific tests (ext2, xhci, gpt, virtio_net, net_stack) still boot separately.

8. **`make clean` does not clean user binaries.** `rm -rf build` leaves `user/*/` artifacts. Must run `for d in user/*/; do make -C "$d" clean; done` for a truly clean rebuild. Stale dynamic .elf files from before a Makefile change (e.g., switching static↔dynamic) will silently be used.

---

## Phase 32 — Forward Constraints

**Phase 32 status: ✅ complete. `make test` passes. `test_pty.py` PASS.**

1. **PTY pool is static (16 pairs, ~138KB BSS).** No dynamic growth.

2. **No TIOCSTI (fake input injection).** Security risk. Deferred.

3. **No packet mode (TIOCPKT).** Not needed for SSH/screen. Deferred.

4. **No virtual consoles.** One console TTY + 16 PTYs.

5. **No SIGHUP on PTY master close.** Only session leader exit triggers SIGHUP. Master close → EIO on slave.

6. **SIGTTIN/SIGTTOU only on controlling terminal.** Background I/O to pipes/files unaffected.

7. **Console output still goes through printk.** The console tty's write_out calls printk per-char. No direct framebuffer/serial write path for the tty layer.

8. **grantpt/unlockpt are security no-ops.** grantpt returns 0 (musl ioctl). unlockpt clears a lock flag. No ownership changes — capability system gates access.

9. **Signal delivery from console ISR preserved.** Ctrl-C/Z/\ on the physical console still deliver signals immediately from the keyboard ISR for responsiveness. PTY slaves deliver signals from tty_read in syscall context.

10. **Kernel BSS must stay under 6MB.** `vmm_init` maps pd_hi[0..2] (0-6MB). If BSS grows past 6MB (e.g., larger PTY pool), add pd_hi[3]. Currently ~4.1MB.

11. **ioctl request codes must be compared as uint32_t.** musl passes ioctl requests as `int`; bit-31-set values (e.g., TIOCGPTN=0x80045430) are sign-extended to 0xFFFFFFFF80045430 in uint64_t arg2. `sys_ioctl` uses `switch ((uint32_t)arg2)`.

12. **PTY master/slave fds are refcounted.** `master_refs`/`slave_refs` track dup/fork inheritance. `master_open`/`slave_open` only cleared when refcount hits 0.

---

## Phase 31 — Forward Constraints

**Phase 31 status: ✅ complete. `make test` passes. `test_proc.py` PASS.**

1. **VMA table has no spinlock.** Safe single-core (syscalls non-preemptible). SMP requires a per-table spinlock.

2. **VMA refcount for CLONE_VM is not locked.** The refcount increment/decrement is safe single-core. SMP needs atomic ops.

3. **`/proc/[pid]/exe` is a plain text file, not a symlink.** Phase 38 (symlinks) can upgrade it.

4. **cmdline stores exe name only, not full argv.** Full argv tracking deferred.

5. **`/proc/[pid]/fd/` entries are plain names, not symlinks.** No target path info.

6. **init's ELF segment VMAs are not tracked.** `proc_spawn` calls `elf_load` before the process is on the run queue, so `sched_current()` returns the idle task. ELF VMAs for init are missing from `/proc/1/maps`. All exec'd processes have correct VMA tracking.

7. **`pmm_free_pages()` scans the entire bitmap.** O(128KB / 8 = 16K iterations) for 128MB. Fast enough. For multi-GB memory, add a running counter.

8. **Procfs allocates 2 kva pages per open (priv + buffer).** Each `/proc` file open costs 8KB of kva. Close frees both. No caching.

---

## Phase 30 — Forward Constraints

**Phase 30 status: ✅ complete. `make test` passes. `test_mmap.py` PASS.**

1. **Freelist has no lock.** Safe on single-core (no preemption during syscalls). SMP requires a spinlock on `mmap_free[]` and `mmap_base`.

2. **PROT_NONE pages still allocate physical frames.** A true demand-paging PROT_NONE (reserve VA only, fault-in on mprotect) is future work. Current approach wastes RAM on guard pages but is simple and correct.

3. **No MAP_FIXED.** sys_mmap rejects addr!=0. MAP_FIXED (map at a specific VA) deferred.

4. **File-backed mmap and MAP_SHARED deferred.** No consumers until Phase 33 (dynamic linking) and Phase 39 (IPC).

5. **PROT_NONE pages leak physical frames on munmap.** `vmm_phys_of_user` only returns phys for PRESENT pages. After `mprotect(PROT_NONE)` clears PRESENT, munmap can't find the phys to free it. Guard pages are small (1 page per thread stack) so the leak is negligible.

6. **Makefile lacks header dependency tracking.** Changes to `.h` files do not trigger recompilation of dependent `.c` files. Always use `rm -rf build && make` (or `make clean && make`) when modifying headers. A `-MMD`/`.d` dep file system should be added.

7. **No user-mode exception → signal delivery.** CPU exceptions (page fault, GPF, etc.) from ring-3 cause kernel PANIC, not SIGSEGV. Hardware exception → signal delivery is future work (needed for proper PROT_NONE guard page behavior).

8. **EFER.NXE enabled globally.** All PTEs with bit 63 set are now enforced as non-executable. Any code that sets PTE bit 63 without NXE was previously silent; now it's enforced.

---

## Phase 29 — Forward Constraints

**Phase 29 status: ✅ complete. `make test` passes. `test_threads.py` PASS.**

### Architecture constraints

1. **`fd_table_t` refcounting has no locking.** Threads sharing an fd table via `CLONE_FILES` can race on `fd_table->fds[]` operations (open/close/dup). No spinlock protects the fd array. Single-core scheduler prevents true parallelism, but a multi-core port requires per-fd-table spinlocks.

2. **Futex pool is 64 static slots.** `futex_waiter_t s_waiters[64]` — if more than 64 threads block on futexes simultaneously, `FUTEX_WAIT` returns `-EAGAIN`. Sufficient for early threading; a hash-table futex implementation is Phase 30+ work.

3. **`mprotect` is a stub.** musl's `pthread_create` calls `mprotect(PROT_NONE)` for the thread stack guard page. Our stub returns 0 (success) but does not actually change page permissions. Guard page protection is Phase 30 (mprotect) work.

4. **Thread stack mmap pages are never freed.** `sys_munmap` is a no-op stub. Thread stacks allocated via `mmap(MAP_ANONYMOUS)` for pthread_create are leaked when the thread exits. Real munmap with VA freelist is Phase 30 work.

5. **`exit_group` kills threads via `sched_exit`.** Each thread in the group is individually removed from the run queue. There is no bulk-kill path — the exiting thread iterates the process list. With many threads this is O(n) per thread × O(n) process scan.

6. **No `CLONE_FS` support.** The `CLONE_FS` flag (shared cwd/umask) is accepted but ignored. Each thread inherits the parent's cwd at clone time but subsequent `chdir` in one thread does not affect others. Full CLONE_FS requires a refcounted `fs_struct`.

7. **Shared signals between threads are not implemented.** `CLONE_SIGHAND` is accepted but signal delivery still targets individual processes. Thread-group-wide signal delivery (e.g., `kill(pid, sig)` hitting any thread in the group) requires Phase 32 (TTY/PTY) work.

8. **`CAP_KIND_THREAD_CREATE` is granted to all exec'd binaries.** Future sandboxing should support revoking this capability via vigil service config (e.g., `-THREAD_CREATE` in capabilities list).

---

## REMINDER: Bare-Metal Testing

**Strongly prefer bare-metal testing between phases.** QEMU hides real bugs. But don't block productive work when hardware is unavailable — bug fixes, cleanup, and small features are fine. The nuclear clean build sequence is the actual non-negotiable for ISOs.
