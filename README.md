# Aegis

A capability-based x86_64 operating system built from scratch. Monolithic kernel, custom GUI stack, full network stack, and fine-grained security enforcement at every syscall boundary.

```
make iso     # build the bootable ISO
make run     # boot in QEMU
make test    # headless boot + serial diff against expected output
```

Aegis is **v1 software** — a first public release, not production-hardened. The C kernel almost certainly contains real, exploitable vulnerabilities. A gradual Rust migration is underway, starting with the capability validation core (`kernel/cap/`). See [`SECURITY.md`](SECURITY.md) for the full security posture.

## Documentation

Full technical documentation lives at **[aegis.byexec.com](https://aegis.byexec.com)**.

### Overview
- [Introduction](https://aegis.byexec.com/) — landing page, project stats, feature index
- [Architecture](https://aegis.byexec.com/docs/architecture/) — system diagram, subsystem map, capability flow
- [Roadmap](https://aegis.byexec.com/docs/roadmap/) — what's next, what's done, what's long-term

### Kernel
- [Boot Process](https://aegis.byexec.com/docs/kernel/boot/) — Multiboot2, mode transitions, higher-half setup
- [Memory Management](https://aegis.byexec.com/docs/kernel/memory/) — PMM, VMM, KVA, VMAs
- [Scheduler](https://aegis.byexec.com/docs/kernel/scheduler/) — round-robin, context switch, wait queues
- [Processes & ELF](https://aegis.byexec.com/docs/kernel/processes/) — fork, exec, dynamic linking via PT_INTERP
- [Syscall Interface](https://aegis.byexec.com/docs/kernel/syscalls/) — 100+ syscalls across 15 categories
- [Interrupts & Exceptions](https://aegis.byexec.com/docs/kernel/interrupts/) — IDT, PIC, LAPIC, exception handlers

### Security
- [Capability Model](https://aegis.byexec.com/docs/security/capabilities/) — per-process capability tables, Rust validation core
- [Security Policy Engine](https://aegis.byexec.com/docs/security/policy/) — `/etc/aegis/caps.d/`, baseline + policy capabilities

### Filesystems
- [VFS Layer](https://aegis.byexec.com/docs/kernel/vfs/) — eight-backend prefix dispatch
- [ext2](https://aegis.byexec.com/docs/kernel/ext2/) — read-write implementation with block cache
- [procfs & special filesystems](https://aegis.byexec.com/docs/kernel/procfs/) — procfs, ramfs, initrd, pipes, memfd

### Networking
- [Network Stack](https://aegis.byexec.com/docs/networking/stack/) — Ethernet, ARP, IPv4
- [TCP/IP](https://aegis.byexec.com/docs/networking/tcp/) — state machine, retransmit, known races
- [Socket API](https://aegis.byexec.com/docs/networking/sockets/) — BSD sockets, epoll, AF_UNIX

### Drivers
- [Driver Overview](https://aegis.byexec.com/docs/kernel/drivers/) — NVMe, xHCI, RTL8169, virtio-net, framebuffer, USB HID

### Graphics
- [Lumen Compositor](https://aegis.byexec.com/docs/gui/compositor/) — direct framebuffer, dirty-rect compositing
- [Glyph Toolkit](https://aegis.byexec.com/docs/gui/toolkit/) — widget library, TrueType rendering
- [Citadel Desktop](https://aegis.byexec.com/docs/gui/desktop/) — top bar, dock, Bastion display manager

### Userspace
- [Vigil Init System](https://aegis.byexec.com/docs/userspace/init/) — PID 1, service supervision
- [Shell & Coreutils](https://aegis.byexec.com/docs/userspace/shell/) — POSIX shell, capability-aware stsh, coreutils
- [Services](https://aegis.byexec.com/docs/userspace/services/) — login, bastion, httpd, dhcp, installer

### Development
- [Build System](https://aegis.byexec.com/docs/build/system/) — Makefile targets, musl build, ISO assembly
- [Testing](https://aegis.byexec.com/docs/build/testing/) — Vortex harness, QEMU integration tests, screenshot diffing

## Contributing

Issues and pull requests welcome at [github.com/exec/aegis](https://github.com/exec/aegis/issues). See [`SECURITY.md`](SECURITY.md) for vulnerability reporting.
