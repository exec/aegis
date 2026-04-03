# Aegis Kernel — Development History

> Preserved from the project-wide and user-wide CLAUDE.md files as of 2026-04-02.
> These files guided Claude Code sessions throughout development. This document
> captures the project's evolution in chronological order.

---

## Origins and Mission

Aegis is a clean-slate, capability-based POSIX-compatible kernel. Not a Linux fork.
The goal: boot real x86-64 hardware, run real software via a musl-compatible ABI,
and enforce a security model where no process — including root — holds ambient
authority.

Every design decision was evaluated against capability isolation. If a shortcut
undermined capability isolation, the shortcut was wrong.

---

## Phase 1 — Boot and Serial (Foundations)

**Boot entry (multiboot2):** GRUB loads via multiboot2; identity-mapped at 0x100000.

**Serial driver (COM1):** 115200 baud, polling 8N1. Emits `\r\n` for terminals.

**VGA text mode driver:** 80x25, ROM font, attr 0x07.

**printk:** serial-first, VGA-conditional. `%u` for unsigned, no `%d`.

**Test harness (make test):** GRUB ISO + ANSI-strip + CRLF-strip + diff; exits 0.

### Phase 1 deviations from original spec

| Item | Spec | Actual | Reason |
|------|------|--------|--------|
| `x86_64-elf-gcc` | native cross-compiler | symlink → `x86_64-linux-gnu-gcc` 14.2 | Not in Debian repos; functionally equivalent with `-ffreestanding -nostdlib` |
| Boot method | QEMU `-kernel aegis.elf` | GRUB + `-cdrom aegis.iso` | QEMU 10 dropped ELF64 multiboot2 detection via `-kernel`; GRUB detects correctly |
| Serial output isolation | direct `diff` | strip ANSI + `grep ^[` + `diff` | SeaBIOS/GRUB write ANSI sequences to COM1 before kernel; our lines all start with `[` |
| QEMU test flags | `-nographic` | `-display none -vga std` | Without VGA device, GRUB falls back to serial, contaminating COM1 with escape sequences even after ANSI stripping |

---

## Phase 2 — Physical Memory Manager

**PMM:** Bitmap allocator; single-page (4KB) only; multi-page deferred to buddy allocator.

---

## Phase 3 — Virtual Memory / Paging

**VMM:** Higher-half kernel at 0xFFFFFFFF80000000; 5-table setup (identity + kernel);
identity map kept; teardown deferred.

**Forward-looking constraint:** Identity map teardown and `zero_page()` had to be
resolved together. `vmm.c`'s `zero_page()` cast physical addresses to pointers —
valid only while the identity window `[0..4MB)` was active. The order was
non-negotiable: mapped-window allocator → tear down identity.

---

## Phase 4 — Scheduler and Interrupts

**IDT:** 48 interrupt gates; isr_dispatch handles PIC EOI before calling handlers.

**PIC:** 8259A remapped; IRQ0-15 → vectors 0x20-0x2F; per-driver unmask.

**PIT:** Channel 0 at 100 Hz; `arch_get_ticks()` accessor; `arch_request_shutdown()` defers exit to ISR.

**PS/2 keyboard:** Scancode→ASCII; ring buffer; blocking `kbd_read()` + non-blocking `kbd_poll()`.

**Scheduler (single-core):** Preemptive round-robin; circular TCB list; `ctx_switch` in NASM; `sched_start` dummy-TCB pattern.

**Constraints carried forward:**
- Identity map still active — TCB and stack allocations cast PMM physical addresses directly to pointers.
- `arch_request_shutdown` was a test-harness hook (QEMU isa-debug-exit), not a real shutdown path.
- Single-core only — `s_current` was a global with no locking.

*Completed 2026-03-20 — make test GREEN.*

---

## Phase 5 — User-Space Process

**GDT (ring-3 descriptors):** User CS 0x23, User DS 0x1B installed.

**TSS:** RSP0 wired to kernel stack top.

**SYSCALL/SYSRET:** IA32_STAR/LSTAR/SFMASK; `syscall_entry.asm` landing pad; o64 sysret.

**ELF loader:** PT_LOAD segments mapped into user PML4 via `vmm_map_user_page`.

**User process (proc_spawn):** Per-process PML4; KSTACK_VA; `proc_enter_user` CR3 switch + iretq.

**Syscall dispatch (C):** sys_write cap-gated (VFS_WRITE) + fd-table dispatch; sys_exit/open/read/close.

**Constraints:**
- SMAP not enabled — sys_write dereferences user addresses directly.
- KSTACK_VA was a single fixed address shared by all user processes.
- `sched_exit` used a master-PML4 switch because TCBs lived in identity-mapped memory.

*Completed 2026-03-20 — make test GREEN.*

---

## Phase 6 — Mapped-Window Allocator

**Mapped-window allocator:** VMM_WINDOW_VA=0xFFFFFFFF80600000; `phys_to_table`/`zero_page` eliminated from vmm.c; identity map still active (teardown in Phase 7).

**Key implementation detail:** The window-map walk pattern was: map once, unmap once — not per-level. One `invlpg` per level (4 total for a 4-level walk), one unmap at the end. `s_window_pte` had to remain `volatile`.

**Identity map teardown order was non-negotiable:**
1. Implement `vmm_window_map` / `vmm_window_unmap`
2. Replace every `phys_to_table()` call with window-mapped access
3. Verify `make test` GREEN
4. Only then clear PML4[0]

*Completed 2026-03-21 — make test GREEN.*

---

## Phase 7 — Identity Map Teardown

KVA bump allocator at `pd_hi[4+]`; physical casts in sched/proc/elf eliminated; PML4[0] cleared.

*Completed 2026-03-21 — make test GREEN.*

---

## Phase 8 — SMAP + User Pointer Validation

CR4.SMAP enabled (Broadwell+); sys_write returns EFAULT for kernel addresses; stac/clac per-byte load.

*Completed 2026-03-21 — make test GREEN.*

---

## Phase 9 — Syscall Cleanup + KVA Free

`syscall_util.h` + `uaccess.h`; `copy_from_user` in sys_write; `kva_free_pages` wired into `sched_exit`.

*Completed 2026-03-21 — make test GREEN.*

---

## Phase 10 — VFS and File Descriptors

VFS + sys_read/open/close; static initrd; `vmm_free_user_pml4`; `stack_pages` field; `task_idle`; `copy_to_user`.

*Completed 2026-03-21 — make test GREEN.*

---

## Phase 11 — Capability System (Rust)

CapSlot + `cap_grant`/`cap_check` Rust FFI; per-process cap table in `aegis_process_t`; sys_open gated on CAP_KIND_VFS_OPEN|CAP_RIGHTS_READ; `docs/capability-model.md` added.

*Completed 2026-03-21 — make test GREEN.*

---

## Phase 12 — Console VFS + Write Capabilities

Console VFS device; fd 1 pre-opened at spawn; CAP_KIND_VFS_WRITE; sys_write capability-gated + routed through fd table.

*Completed 2026-03-21 — make test GREEN.*

---

## Phase 13 — stdin/stderr/sys_brk

kbd VFS driver; fd 0/2 pre-open; CAP_KIND_VFS_READ gate on sys_read; sys_brk (syscall 12); `task_kbd` retired; task count 2.

**Constraints:**
- `sys_brk` page-aligns the break. musl handles the rounded-up return correctly.
- fd 0 blocks on `kbd_read()` — headless `make test` must not call `sys_read(0, ...)`.
- No `sys_mmap` yet — musl's allocator falls back to mmap if brk fails.
- Capability delegation deferred.

*Completed 2026-03-21 — make test GREEN.*

---

## Phase 14 — musl Port

musl-gcc static binary runs via sys_mmap/sys_arch_prctl/sys_writev; SSE init; r8/r9/r10 preserved across syscall; ELF segment alignment fixed; `[INIT] Hello from musl libc!` confirmed.

**Constraints:**
- `mmap_base` was a bump allocator — no free path. `sys_munmap` was a no-op stub.
- `fs_base` not saved/restored on context switch (single process was fine).
- No file-backed mmap, no `mprotect`, no `fork`/`exec`.

*Completed 2026-03-21 — make test GREEN.*

---

## Phase 15 — Interactive Shell

fork/execve/waitpid; interactive shell; 8 companion programs in initrd.

**Post-fix bugs resolved (3):**
1. `fork_child_return` SYSRET path replaced with isr_post_dispatch iretq path — child's first scheduling uses complete fake `isr_common_stub` frame, eliminating r12=0 #PF.
2. `arch_set_fs_base` now set BEFORE `ctx_switch` for incoming task.
3. `sys_open` 256-byte bulk copy replaced with byte-by-byte null-terminated copy — prevents #PF near USER_STACK_TOP.

**Constraints:**
- No `sys_munmap` VA reclaim.
- `sys_chdir` does not validate path existence.
- No I/O redirection, pipes, or signal handling.
- `sys_fork` fd copy without reference counting.

*Completed 2026-03-21 — make test GREEN. test_shell.py all 9 commands clean.*

---

## Phase 16 — Pipes and I/O Redirection

sys_pipe2/dup/dup2; `kernel/fs/pipe.c` ring buffer; shell pipeline parser; 5/5 smoke tests pass.

**Constraints:**
- `O_CLOEXEC` accepted but ignored.
- `SIGPIPE` deferred — writers get `-EPIPE`, not a signal.
- Single waiter per pipe end.

*Completed 2026-03-21 — test_pipe.py 5/5 GREEN.*

---

## Phase 17 — Signals

sigaction/sigprocmask/sigreturn/kill/setfg; Ctrl-C delivery.

---

## Phase 18-21 — PCIe, ACPI, NVMe, ext2

**PCIe + ACPI:** MCFG+MADT on q35; ECAM ≤8 buses.

**NVMe:** NVMe 1.4 blkdev driver.

**ext2:** Read-write; 16-slot LRU block cache; double indirect blocks; symlinks.

---

## Phase 22 — xHCI + USB HID Keyboard

xHCI init on q35+qemu-xhci; USB HID boot protocol; `kbd_usb_inject` into PS/2 ring buffer.

**Constraints:**
- Polling-only at 100Hz from PIT tick handler.
- Single keyboard only. No hub support. No USB mass storage.
- Transfer ring memory never freed.

*Completed 2026-03-23 — test_xhci.py PASS.*

---

## Phase 23 — GPT Partitions

`gpt.c` CRC32 + header validation; nvme0p1/nvme0p2 registered; ext2 mounts nvme0p1.

Custom Aegis GPT partition type GUID family: `A3618F24-0C76-4B3D-xxxx-xxxxxxxxxxxx`.

*Completed 2026-03-23 — test_gpt.py PASS.*

---

## Phase 25-28 — Networking

**virtio-net + network stack:** Ethernet/ARP/IPv4/ICMP/TCP/UDP; `netdev_t`; 12-byte virtio header.

**Socket API + epoll:** socket/bind/listen/accept/connect/send/recv; `sock_t` VFS fds; httpd.

**DHCP + curl:** Userspace RFC 2131; BearSSL+curl static build.

---

## Phase 29 — Threads

clone(CLONE_VM); per-thread TLS; `fd_table_t` refcount; futex.

---

## Phase 30 — mprotect + mmap

W^X enforcement via NX/EFER.NXE; 64-slot VA freelist for proper munmap.

---

## Phase 31 — /proc

VMA tracking; procfs VFS; CAP_KIND_PROC_READ.

---

## Phase 32 — TTY/PTY

16 PTY pairs; sessions; SIGTTIN/SIGTTOU; SIGHUP.

---

## Phase 33 — Dynamic Linking

PT_INTERP+ET_DYN; MAP_FIXED+file-backed mmap; musl `libc.so`.

---

## Phase 34 — Writable Root

ext2-first VFS; `rootfs.img` multiboot2 module; RAM blkdev.

---

## Phase 35 — Installer

Text-mode installer; GPT+GRUB+rootfs copy; UEFI boot support.

---

## Phase 36 — USB HID Mouse

`/dev/mouse` VFS device; boot protocol; xHCI device type detection.

---

## Phase 37 — Lumen Compositor

Backbuffer composite; z-order windows; PTY terminal; taskbar. First GUI.

---

## Phase 38 — SMP

LAPIC+IOAPIC; ~30 spinlocks; SWAPGS; AP trampoline; TLB shootdown.

**Bare-metal results (ThinkPad X13 Gen 1, Zen 2):** Lumen worked. IOAPIC and framebuffer capability fixes required. Performance needed work.

**Key learning:** SWAPGS omitted from `proc_enter_user` as AMD Zen 2 workaround.

---

## Phase 39 — Glyph Widget Toolkit

`libglyph.a` widget toolkit; dirty-rect compositor optimization.

---

## Phase 40 — Citadel Desktop + sys_spawn

sys_spawn (syscall 514); lumen terminal via spawn (no fork); desktop icons.

**Critical decision:** Terminal processes are NEVER embedded via fork inside the compositor. Always use sys_spawn. This was learned the hard way — a global `vfs_read_nonblock` race caused lumen freezes. Per-task flag fixed it.

---

## Phase 41 — Symlinks + chmod/chown

ext2 symlinks; DAC enforcement. No uid=0 bypass (by design).

---

## Phase 42 — stsh (Styx Shell)

CAP_DELEGATE/QUERY; `caps`/`sandbox` builtins; line editing; tab completion.

Custom shell built for Aegis rather than using oksh for GUI terminal.

---

## Phase 42b — Quiet Boot + Lumen Fixes

`printk_quiet`; dual-ISO test harness (graphical and text modes).

---

## Phase 44 — IPC

AF_UNIX SOCK_STREAM; SCM_RIGHTS; memfd_create; MAP_SHARED.

---

## Phase 45 — capd + sys_cap_grant (Later Retired)

capd daemon + sys_cap_grant(363). This approach was later replaced in Phase 46c.

---

## Phase 46 — Bastion Display Manager

Graphical display manager; `libauth.a` + `libcitadel.a`. Bastion owns the graphical session — Vigil starts Bastion, Bastion authenticates, then spawns Lumen.

---

## Phase 46b — GUI Polish

Dark mode, frosted glass, TTF fonts (Terminus 10x20), sys_reboot.

---

## Phase 46c — Capability Policy Redesign

Major redesign of the capability system:

- **Two-tier kernel policy:** Service caps (unconditional) + admin caps (require authenticated session).
- Policy files in `/etc/aegis/caps.d/<binary>` declare caps per-binary, read at execve time.
- `sys_auth_session` (syscall 364) sets `proc->authenticated` after login verifies credentials.
- **Retired:** `sys_cap_grant_exec` (361), `sys_cap_grant_runtime` (363), capd daemon, Vigil `exec_caps` mechanism.
- Baseline caps (hardcoded, all processes): VFS_OPEN, VFS_READ, VFS_WRITE, IPC, PROC_READ(read), THREAD_CREATE.

*Completed 2026-04-02.*

---

## GUI Architecture — Lumen/Glyph/Citadel/Bastion

| Codename | Role | Description |
|----------|------|-------------|
| **Lumen** | Display server + compositor | Owns framebuffer. Composites windows. Dispatches events. |
| **Glyph** | Widget toolkit library | `libglyph.so` — buttons, labels, text input, window chrome, layout. |
| **Citadel** | Desktop shell | `libcitadel.a` linked into Lumen. Taskbar, app launcher, desktop icons, clock. |
| **Bastion** | Display manager | Graphical login + lock screen. Owns session lifecycle. |

**Font:** Terminus bitmap (SIL OFL 1.1), `ter-u20n` (10x20) — 96 columns x 54 rows at 1080p.

---

## Build Toolchain

| Tool | Purpose |
|------|---------|
| `x86_64-elf-gcc` (→ `x86_64-linux-gnu-gcc` 14.2) | Kernel C cross-compiler |
| `nasm` | Assembler |
| `rustup` + nightly + `rust-src` | Rust cap/ subsystem |
| `qemu-system-x86_64` | Emulator |
| `grub-mkrescue` (grub-pc-bin) | Bootable ISO |
| `xorriso` | Required by grub-mkrescue |
| `musl-gcc` (musl-tools) | User binary cross-compiler |
| `sgdisk` (gdisk) | GPT partition tables |
| `e2tools` + `e2fsprogs` | ext2 image population |

---

## Persistent Deviations from Spec

| Item | Spec | Actual | Reason |
|------|------|--------|--------|
| `x86_64-elf-gcc` | native cross | symlink → `x86_64-linux-gnu-gcc` 14.2 | Not in Debian repos |
| Boot method | QEMU `-kernel` | GRUB + `-cdrom` | QEMU 10 dropped ELF64 multiboot2 |
| Serial isolation | direct diff | strip ANSI + strip CRLF + `grep ^[` + diff | SeaBIOS/GRUB contaminate COM1 |
| QEMU test flags | `-nographic` | `-display none -vga std` | GRUB needs VGA device |
| Ring-3 SS selector | 0x1B (RPL=3) | 0x18 (RPL=0) on AMD | AMD 64-bit strips SS RPL bits |

---

## Aegis GPT Partition Type GUIDs

Prefix: `A3618F24-0C76-4B3D-xxxx-xxxxxxxxxxxx`

| Full GUID | Role |
|-----------|------|
| `A3618F24-0C76-4B3D-0001-000000000000` | Root (ext2, mounted as `/`) |
| `A3618F24-0C76-4B3D-0002-000000000000` | Swap (reserved) |
| `A3618F24-0C76-4B3D-0003-000000000000` | Home (reserved) |
| `A3618F24-0C76-4B3D-0004-000000000000` | Data (reserved) |

---

## Key Lessons Learned

1. **QEMU hides real bugs.** IOAPIC timing, AMD SS RPL, fork page copy performance — all only surfaced on bare metal.

2. **Stale git-tracked binaries survive `make clean`.** The catastrophic debugging session (2026-03-28) was caused by stale binaries, not by implementing too many phases. Nuclear clean build sequence: `git clean -fdx`, remove tracked binaries, then build.

3. **Never embed terminal via fork inside compositor.** Global `vfs_read_nonblock` race caused lumen freezes. Per-task flag + sys_spawn was the fix.

4. **capd was overengineered.** A userspace daemon managing capabilities added complexity. Two-tier kernel policy (Phase 46c) was simpler and more secure.

5. **Test suite speed matters.** 15 QEMU boots is too slow. Consolidate tests sharing boot config.

---

## Architecture Audit Open Items (as of 2026-03-29)

### HIGH
- **C5:** SYSRET signal delivery saves incomplete registers — callee-saved (rbx, rbp, r12-r15) corrupted after signal handler. ISR path correct. (signal.c:360)

### MEDIUM
- **M1:** `sched_wake` has no memory barrier — safe on x86, broken on ARM64. (sched.c:352)
- **M2:** mmap freelist has no lock for CLONE_VM threads — safe single-core, corruption on SMP. (sys_memory.c)
- **M3:** `vmm_window_lock` > `pmm_lock` ordering fragile — undocumented. (vmm.c)

### Lock Ordering (Canonical)
```
sched_lock > (all others)
vmm_window_lock > pmm_lock > kva_lock
tcp_lock before sock_lock (deferred wake pattern)
ip_lock before arp_lock (copy-then-release pattern)
```

---

## Phase Roadmap (remaining as of 2026-04-02)

| Phase | Content | Status |
|-------|---------|--------|
| 47 | GUI installer (graphical Glyph version) | Not started |
| 48 | Super key + extended keyboard (PS/2 E0) | Not started |
| 49 | TCP polish (send segmentation, flow control) | Not started |
| 50 | TinySSH + sftp-server (NaCl crypto, key-only) | Not started |
| 51 | Timers (setitimer/alarm/timerfd) | Not started |
| 52 | virtio-blk driver | Not started |
| 53 | Deep security audit | Not started |
| 54 | Release | Not started |
| — | HDA audio | Post-release |
| — | RTL8125 2.5GbE | Post-release |

---

## Test Hardware

- **Dev machine:** ASUS Ryzen 7 6800H, RTL8125B, Wi-Fi MT7921K
- **Test machine A:** Ryzen 7 6800H, dual RTL8168, Intel AX200 Wi-Fi, Samsung NVMe
- **Test machine B (ThinkPad X13 Gen 1):** Ryzen 7 Pro 4750U (Zen 2 / Renoir)
