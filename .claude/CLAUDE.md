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
| curl HTTPS e2e | ✅ | DNS + TCP + TLS 1.2 (BearSSL ChaCha20-Poly1305) + HTTP/1.1; test_curl.py PASS (-k: CA bundle loading TBD) |
| Thread support (Phase 29) | ✅ | clone(CLONE_VM); per-thread TLS; fd_table_t refcount; futex WAIT/WAKE; tgid; clear_child_tid; test_threads.py PASS |
| mprotect + mmap freelist (Phase 30) | ✅ | Real mprotect (W^X via NX/EFER.NXE); 64-slot VA freelist (best-fit + coalescing); test_mmap.py PASS |
| /proc filesystem (Phase 31) | ✅ | VMA tracking (170-slot kva table); procfs VFS; /proc/self/maps,status,stat,exe,cmdline,fd; /proc/meminfo; CAP_KIND_PROC_READ; test_proc.py PASS |
| TTY/PTY layer (Phase 32) | ✅ | Shared tty_t line discipline; 16 PTY pairs (/dev/ptmx + /dev/pts/N); sessions (sid); setpgid relaxed; SIGTTIN/SIGTTOU; SIGHUP on session exit; test_pty.py PASS |
| Dynamic linking (Phase 33) | ✅ | PT_INTERP+ET_DYN; MAP_FIXED+file-backed mmap; musl libc.so; vigil+login static; test_dynlink.py PASS |

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
| 34 | Writable root — ramfs populated from initrd; full live-system writability; foundation for installer | Not started |
| 35 | Installer — text-mode; partition NVMe, format ext2, copy ramfs tree, install GRUB | Not started |
| 36 | Framebuffer / VESA | Not started |
| 37 | AMD Display Core | Not started |
| 38 | **Symlinks + chmod/chown** — VFS symlink resolution; file permission enforcement at VFS layer | Not started |
| 39 | **IPC** — SysV shm/sem/msg; Unix domain sockets; POSIX shared memory; all capability-gated | Not started |
| 40 | **Timers** — setitimer/alarm/timerfd; POSIX interval timers; nanosleep via sched_block (replace busy-wait) | Not started |
| 41 | Release | Not started |
| 42 | RTL8125 2.5GbE driver (PCI 10ec:8125) — post-release, requires WiFi confirmed working | Not started |

**RTL8125 testing note:** The machine has RTL8125B (ASUS, PCI 0a:00.0, IOMMU group 18) managed by host `r8169`. WiFi is MT7921K (0b:00.0). Do NOT test RTL8125 until WiFi is confirmed working — you will lose remote access.

**Test hardware:**
- **Dev machine (this one):** ASUS with Ryzen 7 6800H, RTL8125B (10ec:8125, PCI 0a:00.0), Intel Wi-Fi MT7921K
- **Test machine A:** Ryzen 7 6800H machine (TBD), dual RTL8168 (10ec:8168, PCI 02:00.0 and 03:00.0), Intel AX200 Wi-Fi, Samsung NVMe (144d:a80c, PCI 01:00.0) — for RTL8168 driver testing
- **Test machine B (ThinkPad X13 Gen 1):** Ryzen 7 Pro 4750U (Zen 2 / Renoir)
- **Shared panic (both test machines A and B) — FIXED 2026-03-24:** `[PANIC] corrupt ring-3 iretq frame vec=32 ss=0x18 rsp=0x7ffffffe7d0` after `[SHELL] Aegis shell ready`. Root cause: AMD CPUs in 64-bit long mode strip RPL bits from SS (pushing 0x18 instead of 0x1B on interrupt entry) since SS is unused for addressing in 64-bit mode. Two-part fix: (1) relaxed panic check from `ss != 0x1B` to `(ss & ~3) != 0x18`; (2) added SS RPL normalization in `isr_dispatch` — `if (s->cs == 0x23) s->ss |= 3;` forces RPL=3 before iretq so AMD machines don't #GP. **AMD 64-bit behavior: SS RPL bits are not maintained; expect ss=0x18 on interrupt from ring-3 on AMD, ss=0x1B on Intel/QEMU.**

---

## Phase 27 — Forward Constraints

**Phase 27 status: 🔶 Partial. Infrastructure done; curl HTTPS end-to-end fails.**

**What is done:**
- DHCP daemon: RFC 2131 state machine; vigil service; `[DHCP] acquired` on boot; `test_vigil.py` PASS
- Writable `/etc` + `/root`: multi-instance `ramfs_t`; `initrd_iter_etc` populates etc ramfs at mount
- BearSSL 0.6 + curl 8.x: `tools/fetch-bearssl.sh` + `tools/build-curl.sh`; statically linked; written to ext2 via `make disk`
- `ext2_execve` VFS fallback: `sys_execve` tries initrd first, then `vfs_open` → `kva_alloc` → `ext2_read` → `elf_load`
- `ext2_block_num` double indirect: `i_block[13]`; covers files up to ~67 MB
- `vfs.c` ext2 stat: reads actual `inode.i_mode` from disk instead of hardcoded `0644`
- `test_curl.py`: boots with QEMU monitor socket + keyboard injection; logs in; waits for DHCP; runs curl

**curl HTTPS — RESOLVED.** NET_SOCKET cap was added to execve baseline (line 857). curl works end-to-end with `-k` (skip cert verification). BearSSL CA bundle loading from ext2 fails with error 26 even though the file is accessible (cat reads all 226KB). This is a BearSSL/musl interaction issue, not a kernel bug. CA bundle verification is deferred.

---

*Last updated: 2026-03-27 — Phase 33 (dynamic linking) complete. PT_INTERP+ET_DYN in elf_load, MAP_FIXED+file-backed mmap, musl libc.so on ext2, initrd slimmed to 16 files (vigil+login+config). Pending remote verification.*

---

## Phase 33 — Forward Constraints

**Phase 33 status: ✅ complete. `make test` passes. `test_dynlink.py` PASS.**

1. **No ASLR.** Interpreter loads at fixed INTERP_BASE (0x40000000). PIE binaries load at fixed base. ASLR is future work. Debug builds must disable ASLR to keep `make sym` working.

2. **dlopen/dlsym untested.** The MAP_FIXED + file-backed mmap infrastructure supports it, but no test exercises dlopen. Future work.

3. **curl statically links BearSSL.** Only libc is shared. libbearssl.so is future work.

4. **No LD_PRELOAD or LD_LIBRARY_PATH.** Interpreter uses built-in /lib path only.

5. **File-backed mmap is read-once.** No page cache, no demand paging. Each mmap reads fresh from disk.

6. **Initrd contains only vigil + login + init.** All other binaries on ext2. If ext2 fails to mount, only vigil + login are available.

7. **No MAP_SHARED.** MAP_PRIVATE file-backed mappings only.

8. **Initrd file count changed from 38 to 16.** `tests/expected/boot.txt` updated accordingly.

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
