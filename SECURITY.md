# Security

Aegis is v1 software -- a first public release, not a production-hardened system. The kernel is predominantly written in C (~346K LOC), and there are almost certainly real, exploitable vulnerabilities that have not yet been found. All vulnerabilities identified in audits to date have been hypothetical. This is expected for a from-scratch OS at this stage of maturity.

This document describes where the project stands, what's known to be risky, and how to report issues.

## Reporting Vulnerabilities

Aegis is not running in production anywhere, so private disclosure is not critical -- but we offer both options:

- **Public:** Open an issue at [exec/aegis](https://github.com/exec/aegis/issues). This is fine for most findings.
- **Private:** Email [security@exec.dev](mailto:security@exec.dev) if you prefer coordinated disclosure or if the issue is particularly sensitive.

Include enough detail to reproduce the issue: affected subsystem, triggering input or sequence, and (if applicable) the capability kind and rights bitfield involved. Proof-of-concept code or QEMU reproduction steps are appreciated but not required.

## What's in Scope

Anything in the Aegis codebase is fair game:

- **Kernel** -- memory management, scheduler, VFS, page tables, interrupt handling, syscall dispatch
- **Capability system** -- `cap_check`, `cap_grant`, capability table manipulation, the Rust/C FFI boundary in `kernel/cap/`
- **Syscall interface** -- all 150+ syscalls, argument validation, user-kernel memory copies
- **Security policy engine** -- policy file parsing in `kernel/cap/cap_policy.c`, baseline capabilities, two-tier grant logic
- **Drivers** -- NVMe, xHCI, USB HID, virtio-net, RTL8169, framebuffer
- **Network stack** -- Ethernet, ARP, IP, TCP, UDP, ICMP, BSD socket implementation
- **Userspace services** -- Vigil init, login/authentication flow, stsh shell, httpd, DHCP client

## Known Areas of Concern

These are areas where the project is aware of elevated risk. They are not exhaustive -- a C kernel of this size has a large attack surface.

### C memory safety across ~346K LOC

The most likely class of real vulnerability is a memory safety bug in the C kernel code -- buffer overflows, use-after-free, integer overflows, null dereference in unexpected paths. The capability validation core (`cap_check`, `cap_grant`, `cap_init`) is implemented in Rust with bounds checking and null safety, but everything surrounding it -- syscall handlers, VFS operations, the scheduler, drivers -- is C. A memory corruption bug in any syscall handler could overwrite a process's capability table in kernel memory, bypassing the Rust validation entirely.

### No IOMMU

Aegis does not currently program an IOMMU. A malicious or buggy PCI device (or a compromised driver) could DMA directly into kernel memory, including capability tables. This is a known gap -- IOMMU support is planned but not yet implemented.

### ELF parser handling untrusted input

The ELF loader in `sys_exec` processes untrusted binaries. Malformed ELF headers, overlapping PT_LOAD segments, or crafted dynamic linking metadata could trigger bugs in the loader. The loader validates basic structure but has not been fuzz-tested.

### TCP stack

The TCP implementation handles untrusted network input. State machine edge cases, malformed options, sequence number wrapping, and reassembly logic are all potential vulnerability surfaces. The stack has been functionally tested but not subjected to adversarial traffic.

### User-kernel memory boundary

User pointer validation uses `copy_from_user`/`copy_to_user` with SMAP enforcement, but there is no fault recovery table (`extable`). A malformed user pointer that passes validation but crosses into an unmapped page causes a kernel panic rather than returning `-EFAULT`. This is a known v1 limitation.

### Basename-based policy matching

The security policy engine matches policies on executable basename, not full path. A binary at `/tmp/malicious/login` would match the `login` policy and receive its capabilities (e.g., `CAP_KIND_AUTH`, `CAP_KIND_SETUID`). This is mitigated by filesystem permissions and controlled spawn paths in Vigil, but it is a real design trade-off documented in the [policy engine docs](https://aegis.exec.dev/docs/security/policy/).

## Rust Migration

The capability validation core (`kernel/cap/src/lib.rs`) is the first kernel subsystem written in Rust. It is compiled as a `#![no_std]` staticlib and linked into the C kernel via FFI. This covers `cap_check`, `cap_grant`, and `cap_init` -- the functions that every privileged syscall depends on.

The plan is a gradual, subsystem-by-subsystem migration of safety-critical kernel code from C to Rust, starting with `kernel/cap/` and expanding outward. The FFI pattern established there -- `#![no_std]` staticlib crates with `extern "C"` exports -- serves as the template for future conversions. Expanding the Rust boundary reduces the attack surface for memory corruption bugs that could undermine the capability model's security properties.

This is a long-term effort. Contributions to the Rust migration are welcome.

## Contributing

If you find a bug, have a hardening suggestion, or want to help with the Rust migration, file an issue or open a pull request at [https://github.com/exec/aegis/issues](https://github.com/exec/aegis/issues).
