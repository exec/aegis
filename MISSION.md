Read MISSION.md and .claude/CLAUDE.md in full before proceeding.

# Aegis

**A clean-slate, capability-based operating system.**

Aegis is not a Linux fork. It is not a research toy. It is a complete operating
system — kernel, drivers, filesystem, network stack, GUI compositor, shell —
built from scratch in C and Rust for x86-64, with one defining principle:

**No process ever holds ambient authority.**

Every capability is an unforgeable token, explicitly granted, explicitly scoped.
There is no superuser. `uid=0` is cosmetic. A process that is not granted a
capability simply cannot perform the action — no override, no bypass, no escape
hatch. The kernel enforces this unconditionally.

---

## The Problem

Every major operating system shares a fatal assumption: that identity determines
authority. If you are root, you can do anything. If a process runs as a
privileged user, it inherits every permission that user has ever been granted.

This is why a single compromised service can escalate to full system control.
This is why containers exist — not because the kernel's security model is
strong, but because it is fundamentally broken and must be papered over with
namespace isolation.

Aegis rejects this entirely.

---

## The Security Model

### Capabilities, Not Permissions

A capability in Aegis is an unforgeable token stored in a per-process kernel
table. Capabilities are:

- **Explicitly granted** — a process starts with exactly the capabilities its
  parent (or the init system) chose to give it. Nothing more.
- **Non-delegable by default** — a process cannot grant capabilities it holds
  to a child unless it possesses `CAP_KIND_CAP_DELEGATE`.
- **Irrevocable by the holder** — once granted, capabilities persist for the
  process lifetime. A process cannot escalate its own authority.
- **Enforced at every syscall boundary** — the kernel checks the calling
  process's capability table before any privileged operation. No valid token,
  no action. The error is `ENOCAP`, not `EPERM`.

### Defense in Depth: Capabilities + DAC

Capabilities gate *whether you can attempt an operation*. POSIX DAC (file mode
bits, uid/gid) gates *which specific files you can touch*. Both checks must
pass. A process with `CAP_KIND_VFS_OPEN` can open files — but only files whose
mode bits permit it for that uid/gid. Neither layer alone is sufficient.

Critically: **uid=0 does not bypass DAC checks.** Owner, group, and other bits
are enforced identically for every uid. Root cannot read a file marked 0600
owned by another user. This is the Aegis guarantee — there is no ambient
authority, not even for root.

### The Control Plane: stsh

The fundamental question for any capability-based system is: *who manages the
capabilities?* If root has no special authority, how does an operator configure
the system?

Aegis answers this with **stsh** (the Styx shell) — a capability-aware
management shell that serves as the system's control plane.

```
vigil (init) -> login (authenticates operator) -> stsh (grants CAP_DELEGATE + CAP_QUERY)
vigil (init) -> sshd  (authenticates operator) -> stsh (grants CAP_DELEGATE + CAP_QUERY)
```

stsh is the *only* interface through which an operator can:

- **Query** the capability set of any process (`caps` builtin)
- **Restrict** capabilities when spawning a child (`sandbox` builtin)
- **Inspect** the capability configuration of system services
- **Modify** Vigil service descriptors that define per-service capability grants

These operations require `CAP_KIND_CAP_DELEGATE` and `CAP_KIND_CAP_QUERY` —
capabilities that are **never in the exec baseline**. They are granted
exclusively by the login chain through Vigil service configuration.

An attacker who achieves code execution — even as uid=0 — lands in `/bin/sh`,
the basic POSIX shell. No `CAP_DELEGATE`. No `CAP_QUERY`. No ability to read or
modify capability sets. No ability to edit Vigil configs (DAC blocks write).
Root is a dead end.

**stsh is not a shell. It is the trusted management interface for the entire
operating system.** The only path to it is authentication through a supervised,
capability-gated login chain.

### Vigil: Capability-Aware Init

Vigil is the init system and service supervisor. Every system service runs under
a Vigil service descriptor that specifies:

- The binary to execute
- The capability set to grant (and *only* that set)
- Restart policy (respawn, oneshot, manual)
- Execution mode (direct exec preserves caps; shell wrapper loses them)

Vigil is the root of the capability tree. It holds every capability kind and
delegates subsets to each service. A web server gets `CAP_KIND_NET_SOCKET` and
`CAP_KIND_VFS_READ`. A disk utility gets `CAP_KIND_DISK_ADMIN`. Neither gets
what the other has. Compromise of one service reveals nothing about the other's
authority.

### Dynamic Capability Delegation: Capability Helpers

Static capability grants at spawn time are a simplification. Real workloads
need capabilities on demand — a service that only needs network access during
a sync window, or disk access only when processing a specific request.

Aegis solves this with **capability helper daemons** — small, purpose-built
processes that hold `CAP_KIND_CAP_DELEGATE` and grant capabilities to their
assigned service at runtime, on request, after validation.

```
httpd needs CAP_NET_SOCKET for an outbound request
  -> sends authenticated request to its capability helper
  -> helper validates (kernel-attested PID + challenge-response)
  -> helper calls sys_cap_grant(target_pid, CAP_NET_SOCKET)
  -> httpd now has the capability
```

This is the same pattern as Linux's Polkit — but on a correct foundation.
Polkit mediates privilege escalation because Linux's permission model is
fundamentally ambient. Polkit asks "should I let this process temporarily act
as root?" — which is the wrong question. An Aegis capability helper asks
"should I grant this specific, scoped, unforgeable token to this specific
process for this specific purpose?" — which is the right question.

**Key security properties:**

- **Kernel-attested identity.** The helper learns who is asking from the kernel
  (SO_PEERCRED on the Unix domain socket), not from the requester. A hijacked
  process cannot lie about its PID.
- **Challenge-response validation.** A compromised service that controls the
  socket channel still cannot replay or forge a valid grant request. The helper
  injects a nonce at spawn time; the service must prove it holds the nonce.
- **Narrow scope.** Each helper is purpose-built for one service. The httpd
  helper can only grant network capabilities — it does not hold disk
  capabilities, so it cannot grant them even if tricked.
- **Auditable.** Every grant is logged. stsh can query the audit trail. The
  operator sees exactly when and why every capability was delegated.

**Aegis ships `capd` — the smallest correct policy engine.** capd is a minimal
capability broker daemon supervised by Vigil. It reads declarative policy files
(one per service, human-readable in 5 seconds), handles grant requests over a
Unix domain socket with kernel-attested peer credentials, and logs every
delegation. All built-in Aegis services use capd for dynamic capability grants.

capd is the default, not the mandate. The broker path is configured in
`/etc/aegis/capd.conf` — a file writable only through an authenticated stsh
session (DAC + `CAP_DELEGATE` required). Change the path, change the engine:

```
broker = /bin/capd              # default — declarative policy files
broker = /opt/vendor/capd-hsm   # vendor-supplied, HSM-backed attestation
broker = /usr/local/bin/my-capd # custom, application-specific logic
```

Vigil supervises whatever binary the config points to. The kernel does not know
or care what the broker is — it enforces that `sys_cap_grant` requires
`CAP_DELEGATE`, period. The socket protocol is the contract, not the binary.
capd handles the common case correctly. You build whatever you want.

---

## Defaults, Not Mandates

Every userspace component in Aegis is a default implementation behind a stable
interface. The kernel enforces the contracts — capability checks, socket
protocols, syscall semantics. Everything above the kernel is replaceable.

| Layer | Default | Interface | Replace by |
|-------|---------|-----------|------------|
| Init | Vigil | Supervises services, holds root capabilities | Any binary at `/init` |
| Broker | capd | Socket protocol + `CAP_DELEGATE` | Path in `/etc/aegis/capd.conf` |
| Shell | stsh | Receives caps from login, manages system | Shell field in `/etc/passwd` |
| Auth | login | Authenticates, spawns session with caps | Vigil service config |
| Compositor | Lumen | Owns framebuffer, composites windows | Vigil service config |
| Toolkit | Glyph | Shared library, C API | Link against whatever you want |

Someone builds a better broker. Someone else builds a better shell for that
broker. Someone else builds a better compositor that integrates with both.
The interfaces don't change. The ecosystem grows because every component is a
default, not a dependency.

Even the kernel is a reference implementation. The contract is the syscall ABI
and the capability semantics. A sufficiently motivated developer could write a
completely different kernel — microkernel, unikernel, whatever — and run the
entire Aegis userspace unmodified, as long as the contract holds. The userspace
doesn't know or care what's below the syscall boundary, just as the kernel
doesn't know or care what's above it.

The codebase is a monorepo today — kernel, userspace, and tools co-evolve in
one tree. This is correct while interfaces are still stabilizing and
cross-cutting changes (new syscall + kernel impl + userspace consumer) need to
be atomic. But the endgame is separation: each component becomes its own
project, its own repo, its own release cycle, its own contributors. Bound by
the syscall ABI and the socket protocols, not by sharing a git tree.

This is how a small project becomes a platform.

---

## What Exists Today

Aegis is not a specification. It is running software.

### Kernel (C + Rust, ~174 files)

- **Boot:** Multiboot2 via GRUB, UEFI + legacy BIOS, boot splash
- **Memory:** Physical page allocator, higher-half virtual memory, KVA allocator,
  per-process address spaces, mprotect (W^X via NX bit), mmap freelist
- **Scheduling:** Preemptive round-robin, SMP (multi-core via LAPIC/IOAPIC),
  per-CPU GDT/TSS, TLB shootdown, SWAPGS
- **Processes:** fork, execve, waitpid, sys_spawn (forkless process creation),
  clone (threads), futex, exit_group, sessions, process groups
- **Signals:** sigaction, sigprocmask, sigreturn, kill, Ctrl-C/Z/\, SIGTTIN/SIGTTOU
- **Filesystem:** ext2 (read-write, symlinks, permissions), ramfs, procfs, initrd,
  PTY (16 pairs), pipes, /dev/console, /dev/mouse, /dev/ptmx
- **Network:** virtio-net, Ethernet, ARP, IPv4, ICMP, TCP, UDP, DHCP client
- **Drivers:** NVMe, xHCI (USB 3.0), USB HID keyboard + mouse, PS/2 keyboard,
  framebuffer (VESA/GOP), serial (COM1), PIT, LAPIC timer, IOAPIC, ACPI
- **Security:** Capability system (Rust FFI), SMEP, SMAP, POSIX DAC, W^X enforcement
- **Syscalls:** 80+ Linux-compatible syscall numbers, POSIX socket API, epoll

### Userspace (~41 binaries)

- **Vigil** init system with service supervision and per-service capability grants
- **Login** with password authentication (SHA-512 crypt, /etc/shadow)
- **Shell** (/bin/sh) with pipes, redirection, builtins
- **Lumen** display compositor with backbuffer compositing, window management,
  PTY terminals, mouse cursor, taskbar
- **Glyph** widget toolkit (libglyph.a) with buttons, labels, text fields, windows
- **Network tools:** httpd, DHCP client, curl (with BearSSL for TLS)
- **Core utilities:** ls, cat, cp, mv, rm, mkdir, grep, sort, wc, chmod, chown,
  ln, readlink, touch, clear, pwd, uname, whoami, true, false
- **Installer** for NVMe with GPT partitioning and GRUB installation

### Hardware Tested

- **QEMU:** 25-test automated suite, pc + q35 machine types
- **ThinkPad X13 Gen 1 (Ryzen 7 Pro 4750U, Zen 2):** Full bare-metal boot,
  GUI compositor, USB keyboard + mouse, NVMe, UEFI, ACPI power button

---

## Architecture

### The C/Rust Boundary

The capability subsystem (`kernel/cap/`) is Rust (`no_std`, no `alloc`). The
boundary is a clean FFI interface — `cap_grant`, `cap_check`, `cap_revoke` —
crossed at syscall entry and capability operations. Everything else is C.

### Directory Layout

```
kernel/
  arch/x86_64/    All x86-specific code. Nothing else touches hardware.
  core/           Architecture-agnostic kernel initialization
  mm/             Memory management (PMM, VMM, KVA, page tables)
  cap/            Capability subsystem (Rust, no_std)
  fs/             VFS, ext2, ramfs, procfs, PTY, pipes, devices
  sched/          Scheduler (preemptive, SMP-aware)
  signal/         POSIX signal delivery
  syscall/        Syscall dispatch and implementation
  proc/           Process management
  elf/            ELF loader
  drivers/        NVMe, xHCI, USB HID, virtio-net, framebuffer, ramdisk
  net/            Network stack (Ethernet, ARP, IPv4, TCP, UDP, ICMP)

user/
  vigil/          Init system and service supervisor
  login/          Authentication (SHA-512, /etc/shadow)
  shell/          /bin/sh — basic POSIX shell
  lumen/          Display compositor
  glyph/          Widget toolkit library
  ...             40+ additional binaries
```

### Output Routing

All kernel output goes through `printk()`. Serial first, VGA/framebuffer
second. If VGA fails, serial continues. If serial fails, the system panics.
Panic conditions render a blue screen with register dump and backtrace when a
framebuffer is available.

---

## Strict Constraints

These are enforced throughout development. They are not preferences.

- Architecture-specific code lives ONLY under `kernel/arch/x86_64/`.
  No x86 assumptions anywhere in `kernel/core/`.
- All kernel output goes through `printk()`. No direct hardware writes
  outside the appropriate driver file.
- No external C libraries in kernel code. No `printf`, `malloc`, `free`.
- No floating point. Ever.
- Every `unsafe` Rust block requires: `// SAFETY: <specific reason>`
- `-Wall -Wextra -Werror` — warnings are errors.
- A subsystem is not working because it compiles. It is working when
  `make test` exits 0.

---

## Methodology

1. **Brainstorm before designing.** No subsystem is designed from scratch
   without structured exploration of approaches and trade-offs.
2. **Plan before coding.** Every implementation has a written plan with tasks
   small enough to complete in under 5 minutes each.
3. **Test before implementing.** Write the failing test first. Watch it fail.
   Then implement. If code exists before a test, delete the code and start over.
4. **Review before merging.** Code review after each task. Critical issues block.
5. **Verify before declaring done.** `make test` output is the only evidence.

---

## Roadmap

### Completed

| Phase | Milestone |
|-------|-----------|
| 1-9 | Boot, serial, VGA, printk, PMM, VMM, paging, SMAP, syscalls |
| 10-16 | VFS, capabilities (Rust), shell, musl libc, fork/exec, pipes |
| 22-23 | xHCI USB, GPT partitions, NVMe, ext2 filesystem |
| 25-28 | Network stack, sockets, epoll, DHCP, BearSSL + curl |
| 29-31 | Threads (clone/futex), mprotect (W^X), /proc filesystem |
| 32-33 | TTY/PTY layer, dynamic linking (musl ldso) |
| 34-35 | Writable root (ext2-first VFS), installer, UEFI boot |
| 36-38 | USB mouse, Lumen compositor, SMP (multi-core) |
| 39-41 | Glyph toolkit, sys_spawn, symlinks + chmod/chown + DAC |

### Next

| Phase | Milestone |
|-------|-----------|
| 42 | **stsh** — the Styx shell. Capability-aware control plane. |
| 43 | **IPC** — Unix domain sockets, shared memory, fd passing |
| 44 | **capd** — runtime capability delegation, declarative policy, replaceable broker |
| 45 | **Timers** — setitimer, alarm, timerfd, proper nanosleep |
| 46 | **Bastion** — graphical display manager (login screen) |
| 47 | **GUI installer** — graphical version of text-mode installer |
| 48 | **Release** |

---

*Aegis: no process holds ambient authority. Not even root. Not even the shell —
unless you've proven who you are.*
