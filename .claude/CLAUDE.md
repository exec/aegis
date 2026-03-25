# Aegis Kernel — Claude Code Persistent Instructions

> Read this file completely at the start of every session.
> Do not skip sections. If anything conflicts with a user instruction, follow the user and flag it.

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

6. **Static IP hardcoded.** `net_init()` sets 10.0.2.15/24 gw 10.0.2.2. Phase 28 DHCP daemon will replace this. A future `sys_netcfg` syscall will allow dynamic reconfiguration.

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
| 25 | Ethernet/ARP/IPv4/ICMP/TCP/UDP stack | ✅ Done — test_net_stack.py PASS |
| 26 | POSIX socket API + epoll (`sys_socket`/`bind`/`listen`/`accept`/`connect`/`send`/`recv`/`epoll_*`) | Not started |
| 27 | RTL8125 2.5GbE driver (PCI 10ec:8125) | Deferred until WiFi confirmed working |
| 28 | DHCP userspace daemon | Not started |
| 29 | Framebuffer / VESA | Not started |
| 30 | AMD Display Core | Not started |
| 31 | Installable disk | Not started |
| 32 | Release | Not started |

**RTL8125 testing note:** The machine has RTL8125B (ASUS, PCI 0a:00.0, IOMMU group 18) managed by host `r8169`. WiFi is MT7921K (0b:00.0). Do NOT test RTL8125 until WiFi is confirmed working — you will lose remote access.

**Test hardware:**
- **Dev machine (this one):** ASUS with Ryzen 7 6800H, RTL8125B (10ec:8125, PCI 0a:00.0), Intel Wi-Fi MT7921K
- **Test machine A:** Ryzen 7 6800H machine (TBD), dual RTL8168 (10ec:8168, PCI 02:00.0 and 03:00.0), Intel AX200 Wi-Fi, Samsung NVMe (144d:a80c, PCI 01:00.0) — for RTL8168 driver testing
- **Test machine B (ThinkPad X13 Gen 1):** Ryzen 7 Pro 4750U (Zen 2 / Renoir)
- **Shared panic (both test machines A and B) — FIXED 2026-03-24:** `[PANIC] corrupt ring-3 iretq frame vec=32 ss=0x18 rsp=0x7ffffffe7d0` after `[SHELL] Aegis shell ready`. Root cause: AMD CPUs in 64-bit long mode strip RPL bits from SS (pushing 0x18 instead of 0x1B on interrupt entry) since SS is unused for addressing in 64-bit mode. Two-part fix: (1) relaxed panic check from `ss != 0x1B` to `(ss & ~3) != 0x18`; (2) added SS RPL normalization in `isr_dispatch` — `if (s->cs == 0x23) s->ss |= 3;` forces RPL=3 before iretq so AMD machines don't #GP. **AMD 64-bit behavior: SS RPL bits are not maintained; expect ss=0x18 on interrupt from ring-3 on AMD, ss=0x1B on Intel/QEMU.**

*Last updated: 2026-03-24 — ext2 persistence fixed (stale shell binary); AMD ring-3 SS panic fixed (two-part: relaxed check + SS RPL normalization in isr_dispatch). make test 15/15 PASS.*
