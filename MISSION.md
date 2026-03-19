Read MISSION.md and .claude/CLAUDE.md in full before proceeding.

We are building Aegis — a clean-slate, capability-based POSIX kernel 
in C (Rust for cap/ and IPC subsystems). x86-64, multiboot2, ELF.
The defining principle: no process ever holds ambient authority.

---

## PHASE 1 — COMPLETE (2026-03-19)

Goal was: toolchain + testing harness proven end-to-end.
A multiboot2 kernel boots in QEMU, emits structured serial output,
and a test script diffs that output against expected and exits 
non-zero on mismatch.

What was originally planned vs what was built:

  ORIGINAL PLAN                       ACTUAL
  QEMU -kernel aegis.elf              GRUB + -cdrom aegis.iso
    (QEMU 10 dropped ELF64              (GRUB reliably loads multiboot2;
     multiboot2 detection)               QEMU 10 requires PVH note for
                                         direct ELF64 boot)

  direct diff of serial output        strip ANSI + grep '^[' + diff
    (clean COM1)                        (SeaBIOS + GRUB write ANSI
                                         escape codes to COM1 before
                                         the kernel starts)

  x86_64-elf-gcc (cross-compiler)     symlink -> x86_64-linux-gnu-gcc 14.2
    (not in Debian repos)               (functionally equivalent with
                                         -ffreestanding -nostdlib)

  COM1 at 38400 baud (original ask)   COM1 at 115200 baud (user chose A)

make test exits 0. See .claude/CLAUDE.md Build Status for full details.

---

STRICT CONSTRAINTS — enforce these throughout, non-negotiable:

- Architecture-specific code lives ONLY under kernel/arch/x86_64/
  No x86 assumptions anywhere in kernel/core/
- printk() writes to serial AND VGA simultaneously
  If VGA is unavailable, serial still works. Never the reverse.
- All kernel output goes through printk(). 
  No direct writes to 0xB8000 outside vga.c
  No direct writes to serial port outside serial.c
- Serial output format for every subsystem init:
    [SUBSYSTEM] OK: <message>    or
    [SUBSYSTEM] FAIL: <reason>
  tests/expected/boot.txt must match this exactly
- No external C libraries. This is a kernel.
- No dynamic allocation in early boot (before PMM exists)
- No floating point ever
- Every unsafe Rust block requires a comment: why is this safe?
- The Rust cap/ subsystem in Phase 1 is a STUB ONLY:
    cap_init() prints [CAP] OK: reserved and returns.
    Do not implement capability logic yet. Do not be clever.

---

QEMU invocation for make run (as built):
  qemu-system-x86_64 \
    -machine pc \
    -cdrom build/aegis.iso -boot order=d \
    -serial stdio -vga std \
    -no-reboot -m 128M \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04

QEMU invocation for make test (headless, as built):
  qemu-system-x86_64 \
    -machine pc \
    -cdrom build/aegis.iso -boot order=d \
    -nographic -nodefaults -serial stdio \
    -no-reboot -m 128M \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    > /tmp/aegis_serial_raw.txt 2>/dev/null
  # strip ANSI, keep only lines starting with '['
  sed 's/\x1b\[[?0-9;]*[A-Za-z]//g; s/\x1bc//g' raw.txt \
    | grep '^\[' > /tmp/aegis_serial.txt
  diff tests/expected/boot.txt /tmp/aegis_serial.txt
  # exit non-zero on mismatch

make test must return exit code 0 on success, 1 on failure.
This is the definition of "working" for every phase.

---

For each new phase: after /superpowers:write-plan is complete and
you have shown the plan, STOP and wait for explicit go-ahead before
invoking /superpowers:execute-plan.

The user reviews every plan before any code is written.
