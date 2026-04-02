# Aegis Phase 1 Design — Boot, Output, Test Harness

**Date:** 2026-03-19
**Scope:** Phase 1 only — multiboot2 boot entry, serial driver, VGA driver, printk, CAP stub, test harness
**Status:** Approved

---

## Goals

Prove the toolchain and testing harness end-to-end. A multiboot2 kernel boots in
QEMU, emits structured serial output, and a test script diffs that output against
a known-good file, exiting non-zero on any mismatch.

Every subsequent phase adds subsystems on top of this foundation without
restructuring what is built here.

---

## Decisions

| Question | Decision | Rationale |
|----------|----------|-----------|
| Boot mechanism | Multiboot2 header + QEMU `-kernel` | No GRUB needed for Phase 1; QEMU scans first 8KB for magic |
| COM1 baud rate | 115200 | Modern standard; supersedes the illustrative 38400 in CLAUDE.md examples |
| Load address | 0x100000 (1MB, identity-mapped) | No page table setup needed in Phase 1; VMM phase handles higher-half |
| Test method | Exact diff against `tests/expected/boot.txt` | Deterministic output in Phase 1; catches regressions automatically |
| Phase 1 output | 4 fixed lines (see below) | No PMM/VMM/sched in Phase 1; those add lines in their own phase |
| printk format | String-only, no format specifiers | Nothing needs `%d`/`%x` yet; add when needed |

---

## Expected Output (`tests/expected/boot.txt`)

This file is written before any kernel code. It is the failing test.

```
[SERIAL] OK: COM1 initialized at 115200 baud
[VGA] OK: text mode 80x25
[CAP] OK: capability subsystem reserved
[AEGIS] System halted.
```

Single newline at end of file. No trailing spaces on any line.

---

## Directory Layout

```
aegis/
├── kernel/
│   ├── arch/x86_64/
│   │   ├── boot.asm          # multiboot2 header, long mode setup, entry point, boot stack
│   │   ├── arch.h            # arch-neutral interface: arch_init(), arch_debug_exit()
│   │   ├── arch.c            # implements arch_init() — calls serial_init(), vga_init()
│   │   ├── arch_exit.c       # implements arch_debug_exit() — QEMU isa-debug-exit port I/O
│   │   ├── serial.c          # COM1 driver — all port I/O lives here only
│   │   ├── serial.h          # internal interface for serial driver (used within arch/ only)
│   │   ├── vga.c             # VGA text mode — 0xB8000 lives here only
│   │   └── vga.h             # internal interface for VGA driver (used within arch/ only)
│   ├── core/
│   │   ├── main.c            # kernel_main — includes only arch.h, printk.h, cap.h
│   │   ├── printk.c          # routes to serial + VGA; arch-agnostic
│   │   └── printk.h          # declares void printk(const char *s)
│   └── cap/                  # Rust crate (no_std)
│       ├── Cargo.toml
│       ├── cap.h             # C header declaring cap_init()
│       └── src/lib.rs        # cap_init() stub
├── tests/
│   ├── run_tests.sh          # test harness script
│   └── expected/
│       └── boot.txt          # ground truth for make test
├── tools/
│   └── linker.ld             # linker script
├── docs/
│   └── superpowers/specs/    # design documents
└── Makefile
```

---

## Component Designs

### Boot Entry (`kernel/arch/x86_64/boot.asm`)

NASM, Intel syntax. Sections explicitly named. Every label has a comment covering
its purpose, registers clobbered, and calling convention.

QEMU `-kernel` boots the kernel in **32-bit protected mode** per the multiboot2
spec. The kernel ELF is 64-bit, so `boot.asm` must transition to 64-bit long mode
before calling any C code.

#### Structure

**`.multiboot` section** (forced first by linker script — must land in first 8KB):
- Multiboot2 header: magic `0xE85250D6`, architecture `0` (x86/32-bit protected
  mode entry), header length, checksum, end tag

**`.text` section — `_start` label** (entry point; CPU is in 32-bit PM, paging off):

1. **Set up a temporary GDT** with a 64-bit code descriptor and a 64-bit data
   descriptor. Load it with `lgdt`.
2. **Enable PAE**: set `CR4.PAE` (bit 5).
3. **Set up a minimal identity-mapping page table** covering the first 2MB
   (one PML4 entry → one PDPT entry → one 2MB huge page at physical 0).
   This is enough to keep executing while paging is enabled. Page table
   structures go in `.bss` (zeroed, no dynamic allocation).
4. **Load page table root**: `mov cr3, <address of PML4>`.
5. **Enable long mode**: set `EFER.LME` (bit 8) via `rdmsr`/`wrmsr` on MSR `0xC0000080`.
6. **Enable paging**: set `CR0.PG` (bit 31) and `CR0.WP` (bit 16) simultaneously.
   The CPU is now in compatibility mode (32-bit code in long mode).
7. **Far jump to 64-bit code segment**: `jmp 0x08:.long_mode_entry`. This
   reloads `CS` with the 64-bit code descriptor and activates true 64-bit mode.
8. **`.long_mode_entry`** (64-bit code from here):
   - Set `DS`, `ES`, `SS` to 64-bit data descriptor.
   - Load stack pointer: `mov rsp, boot_stack_top`.
   - Translate multiboot2 registers to System V AMD64 ABI:
     ```nasm
     ; multiboot2 spec: magic in eax, info ptr in ebx
     mov edi, eax      ; first arg (mb_magic) → edi/rdi
     mov rsi, rbx      ; second arg (mb_info) → rsi (zero-extend from ebx)
     ```
   - Call `kernel_main`.
   - Hang loop if `kernel_main` returns (it must not).

**`.bss` section**:
- `boot_stack`: 16KB, 16-byte aligned. `boot_stack_top` label at high end.
- Page table structures: PML4 (4KB), PDPT (4KB), PD (4KB). Each 4KB-aligned.

#### Higher-Half Migration Path (VMM Phase)

When the VMM phase arrives:
1. In `linker.ld`: change VMA from `0x100000` to `0xFFFFFFFF80000000`; keep LMA at `0x100000`.
2. In `boot.asm`: extend the page table to also map `0xFFFFFFFF80000000 → 0x100000`
   (add a second PML4 entry for the upper half). Far-jump into the higher-half
   address before calling `kernel_main`.
3. No other file changes. `kernel/core/` is untouched.

### Linker Script (`tools/linker.ld`)

- Entry point: `_start`
- Load address = link address = `0x100000` (identity-mapped for Phase 1)
- Section order: `.multiboot` → `.text` → `.rodata` → `.data` → `.bss`
- `.multiboot` forced first: `KEEP(*(.multiboot))` before other `.text` input

```
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)
BASE = 0x100000;

SECTIONS {
    . = BASE;
    .multiboot : { KEEP(*(.multiboot)) }
    .text      : { *(.text .text.*) }
    .rodata    : { *(.rodata .rodata.*) }
    .data      : { *(.data .data.*) }
    .bss       : {
        *(COMMON)
        *(.bss .bss.*)
    }
}
```

### Serial Driver (`kernel/arch/x86_64/serial.c` + `serial.h`)

COM1 base port: `0x3F8`. Init sequence (8N1, polling, no interrupts):

1. Disable interrupts: `IER` (base+1) = 0x00
2. Enable DLAB: `LCR` (base+3) = 0x80
3. Set divisor for 115200 baud: divisor = 1; `DLL` (base+0) = 0x01, `DLH` (base+1) = 0x00
4. 8 data bits, no parity, 1 stop bit, clear DLAB: `LCR` = 0x03
5. Enable and clear FIFO: `FCR` (base+2) = 0xC7
6. Assert DTR and RTS: `MCR` (base+4) = 0x0B

`serial_write_char`: spin on transmit-empty bit (`LSR` bit 5) before writing.
`serial_write_string`: loop over chars, stop at `\0`.

After init, `serial_init()` prints `[SERIAL] OK: COM1 initialized at 115200 baud\n`
directly via its own raw write (printk is not yet up).

`serial.h`:
```c
#ifndef SERIAL_H
#define SERIAL_H

void serial_init(void);
void serial_write_char(char c);
void serial_write_string(const char *s);

#endif
```

### VGA Driver (`kernel/arch/x86_64/vga.c` + `vga.h`)

Mode 3 text mode, 80×25, framebuffer at `0xB8000`. ROM font loaded by QEMU
firmware — no font file required.

Cell layout: byte 0 = ASCII character, byte 1 = attribute (`0x07` = light grey
on black). 4000 bytes total (80 × 25 × 2).

State: column and row counters (module-level `static int`). `\n` increments row
and resets column. When row reaches 25, scroll: memmove all rows up by one,
clear bottom row, reset row to 24. No external `memmove` — implement inline or
as a small static helper using pointer arithmetic.

`vga_init()` clears the screen, sets `vga_available = 1`, prints
`[VGA] OK: text mode 80x25\n` directly via its own raw write.

`vga.h`:
```c
#ifndef VGA_H
#define VGA_H

extern int vga_available;   /* set to 1 by vga_init(); checked by printk */

void vga_init(void);
void vga_write_char(char c);
void vga_write_string(const char *s);

#endif
```

### Arch Interface (`kernel/arch/x86_64/arch.h` + `arch.c` + `arch_exit.c`)

`kernel/core/` must not include any arch-specific header by name. The solution:
each architecture provides `arch.h` with an identical interface. The build system
selects the right arch directory via `-Ikernel/arch/x86_64`. When ARM64 is added,
`-Ikernel/arch/arm64` provides its own `arch.h` with the same declarations.
`kernel/core/main.c` never changes.

`kernel/arch/x86_64/arch.h`:
```c
#ifndef ARCH_H
#define ARCH_H

#include <stdint.h>

/* Initialize all arch-specific subsystems (serial, VGA).
 * Must be the first call in kernel_main. */
void arch_init(void);

/* Signal QEMU isa-debug-exit device. exit_code = (value << 1) | 1.
 * Writing 0x01 causes QEMU to exit with process code 3. */
void arch_debug_exit(unsigned char value);

#endif
```

`kernel/arch/x86_64/arch.c` — calls subsystem inits in order:
```c
#include "arch.h"
#include "serial.h"
#include "vga.h"

/* Initialize all x86_64 early subsystems.
 * Clobbers: none (subsystems manage their own state). */
void arch_init(void) {
    serial_init();
    vga_init();
}
```

`kernel/arch/x86_64/arch_exit.c`:
```c
#include "arch.h"

/* Signal QEMU isa-debug-exit to terminate the VM.
 * Clobbers: none. Inline asm writes value to port 0xf4. */
void arch_debug_exit(unsigned char value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"((unsigned short)0xf4));
}
```

`serial.h` and `vga.h` are internal to `kernel/arch/x86_64/` — they are not
included from `kernel/core/`. Only `arch.h` crosses the boundary.

**Design debt note:** `arch.h` currently carries two responsibilities: the true arch
interface (`arch_init`, `arch_debug_exit`) and the output routing declarations
(`serial_write_string`, `vga_write_string`, `vga_available`) needed by `printk`.
The output declarations are x86-specific in semantics even though they are
arch-neutral in name — an ARM64 port with a PL011 UART has no `serial_write_string`.
The correct long-term fix is a thin `kernel/core/output.h` backed by arch-specific
implementations. This refactor belongs at the start of the next phase, not Phase 1.

### `printk` (`kernel/core/printk.c` + `printk.h`)

`kernel/core/printk.h`:
```c
#ifndef PRINTK_H
#define PRINTK_H

void printk(const char *s);

#endif
```

`printk.c` must reach `serial_write_string` and `vga_available`/`vga_write_string`
without including arch-specific headers by name. The solution: expose the output
functions through the same arch-neutral include path. Add to `arch.h`:

```c
/* Output functions used by printk. Implemented in serial.c / vga.c
 * but declared here so kernel/core/printk.c can call them via arch.h. */
extern int vga_available;
void serial_write_string(const char *s);
void vga_write_string(const char *s);
```

`printk.c`:
```c
#include "arch.h"
#include "printk.h"

void printk(const char *s) {
    serial_write_string(s);
    if (vga_available) {
        vga_write_string(s);
    }
}
```

### `kernel_main` (`kernel/core/main.c`)

`main.c` includes only arch-neutral headers. The build system's `-Ikernel/arch/x86_64`
resolves `arch.h` to the correct implementation.

```c
#include <stdint.h>
#include "arch.h"
#include "printk.h"
#include "cap.h"

void kernel_main(uint32_t mb_magic, void *mb_info) {
    (void)mb_magic;   /* validated in PMM phase when memory map is needed */
    /* mb_info is a physical address (guaranteed < 4GB by multiboot2 spec).
     * It equals the virtual address in Phase 1 due to identity mapping.
     * The VMM phase must account for this when remapping — do not treat
     * mb_info as a virtual pointer in future phases without remapping it. */
    (void)mb_info;
    arch_init();      /* serial_init + vga_init, OK lines printed inside */
    cap_init();
    printk("[AEGIS] System halted.\n");
    arch_debug_exit(0x01);   /* signals QEMU to exit with code 3 */
    for (;;) {}              /* in case QEMU exit device is absent */
}
```

### CAP Stub (`kernel/cap/src/lib.rs`)

```rust
#![no_std]

extern "C" {
    // serial_write_string takes `const char *` in C.
    // On x86-64 with GCC, `char` is signed 8-bit and `u8` is unsigned 8-bit;
    // both are 1 byte with identical ABI representation on this target.
    // This declaration is safe to call with a byte-string literal.
    fn serial_write_string(s: *const u8);
}

/// Initialize the capability subsystem.
///
/// Phase 1: stub only. Prints status line and returns.
/// Writes directly to serial rather than through printk because no
/// `printk` FFI wrapper exists yet. This means CAP output does not
/// appear on VGA in Phase 1. Revisit when a `printk` Rust wrapper
/// is designed (post-PMM/VMM).
#[no_mangle]
pub extern "C" fn cap_init() {
    // SAFETY: serial_init() is called before cap_init() in kernel_main,
    // so the serial port is fully initialized. serial_write_string is a
    // simple polling write with no shared mutable state. The pointer
    // points to a valid null-terminated byte string literal in read-only
    // data. `char` and `u8` have identical 8-bit ABI representation on
    // x86-64 with GCC, so the type mismatch between C `const char *` and
    // Rust `*const u8` is safe on this target.
    unsafe {
        serial_write_string(b"[CAP] OK: capability subsystem reserved\n\0".as_ptr());
    }
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
```

`kernel/cap/cap.h`:
```c
#ifndef CAP_H
#define CAP_H

void cap_init(void);

#endif
```

`Cargo.toml`:
```toml
[package]
name = "cap"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["staticlib"]

[profile.dev]
panic = "abort"

[profile.release]
panic = "abort"
```

Built with: `cargo +nightly build --release --target x86_64-unknown-none`

### Test Harness (`tests/run_tests.sh`)

```bash
#!/usr/bin/env bash
set -e

KERNEL=build/aegis.elf
EXPECTED=tests/expected/boot.txt
ACTUAL=/tmp/aegis_serial.txt

# Boot headless. QEMU exits with code 3 when the kernel writes 0x01 to
# port 0xf4 (isa-debug-exit device: exit_code = (value << 1) | 1 = 3).
# timeout prevents make test from hanging forever if the kernel never
# reaches the exit signal (e.g., early panic, missing outb).
timeout 10s qemu-system-x86_64 \
    -kernel "$KERNEL" \
    -nographic \
    -serial stdio \
    -no-reboot \
    -m 128M \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    > "$ACTUAL" 2>/dev/null || true
# || true: QEMU exits 3 on clean kernel exit; set -e would treat that as failure.

diff "$EXPECTED" "$ACTUAL"
# diff exits 0 on match, 1 on mismatch. This is the exit code of make test.
# Argument order: expected first, actual second — diff output shows missing
# lines as `-` and unexpected lines as `+`, matching test harness convention.
```

`make test` calls this script and propagates its exit code. Exit 0 = pass, 1 = fail.

### Makefile

```makefile
CC      = x86_64-elf-gcc
AS      = nasm
LD      = x86_64-elf-ld
CARGO   = cargo +nightly

CFLAGS  = -ffreestanding -nostdlib -nostdinc \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
          -fno-stack-protector \
          -Wall -Wextra -Werror \
          -Ikernel/arch/x86_64 -Ikernel/core -Ikernel/cap

ASFLAGS = -f elf64
LDFLAGS = -T tools/linker.ld -nostdlib

BUILD   = build

ARCH_SRCS = kernel/arch/x86_64/arch.c \
            kernel/arch/x86_64/arch_exit.c \
            kernel/arch/x86_64/serial.c \
            kernel/arch/x86_64/vga.c
CORE_SRCS = kernel/core/main.c \
            kernel/core/printk.c
BOOT_SRC  = kernel/arch/x86_64/boot.asm
CAP_LIB   = kernel/cap/target/x86_64-unknown-none/release/libcap.a

ARCH_OBJS = $(patsubst kernel/%.c,$(BUILD)/%.o,$(ARCH_SRCS))
CORE_OBJS = $(patsubst kernel/%.c,$(BUILD)/%.o,$(CORE_SRCS))
BOOT_OBJ  = $(BUILD)/arch/x86_64/boot.o

ALL_OBJS  = $(BOOT_OBJ) $(ARCH_OBJS) $(CORE_OBJS)

.PHONY: all run test clean

all: $(BUILD)/aegis.elf

$(BUILD)/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BOOT_OBJ): $(BOOT_SRC)
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(CAP_LIB): kernel/cap/src/lib.rs kernel/cap/Cargo.toml
	$(CARGO) build --release \
	    --target x86_64-unknown-none \
	    --manifest-path kernel/cap/Cargo.toml

$(BUILD)/aegis.elf: $(ALL_OBJS) $(CAP_LIB)
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS) $(CAP_LIB)

run: all
	qemu-system-x86_64 -kernel $(BUILD)/aegis.elf \
	    -serial stdio -vga std -no-reboot -m 128M \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04

test: all
	@bash tests/run_tests.sh

clean:
	rm -rf $(BUILD)
	$(CARGO) clean --manifest-path kernel/cap/Cargo.toml
```

---

## Build Flow

1. NASM compiles `boot.asm` → `build/arch/x86_64/boot.o`
2. GCC compiles all `.c` files → object files under `build/`
3. `cargo +nightly build --release --target x86_64-unknown-none` → `libcap.a`
4. `x86_64-elf-ld` links all objects + `libcap.a` with `tools/linker.ld` → `build/aegis.elf`
5. `make test` boots `aegis.elf` in QEMU headless, diffs serial output against `tests/expected/boot.txt`

---

## What Phase 1 Does NOT Do

- No memory map parsing (PMM phase)
- No paging beyond the minimal identity map needed to reach 64-bit mode (VMM phase)
- No interrupts or IDT (scheduler phase)
- No heap allocation anywhere
- No capability logic (stub only; full implementation after PMM + VMM)
- No format specifiers in printk
- No `printk` Rust FFI wrapper (CAP writes directly to serial in Phase 1)

---

## Constraints Carried Forward

All constraints from CLAUDE.md apply throughout:
- `-Wall -Wextra -Werror` — warnings are errors
- No x86 assumptions outside `kernel/arch/x86_64/`
- No `printf`, `malloc`, `free`, or libc in kernel code
- Every `unsafe` Rust block requires a specific `// SAFETY:` comment
- A subsystem is working only when `make test` exits 0
