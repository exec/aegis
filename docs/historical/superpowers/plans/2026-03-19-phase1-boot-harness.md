# Phase 1 — Boot, Output, Test Harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove the toolchain end-to-end — a multiboot2 kernel boots in QEMU, emits four structured serial lines, and `make test` diffs them against `tests/expected/boot.txt`, exiting 0 on exact match.

**Architecture:** NASM multiboot2 header + 32→64-bit long mode setup in `boot.asm`; C serial and VGA drivers confined to `kernel/arch/x86_64/`; arch-agnostic `printk` in `kernel/core/`; Rust `no_std` CAP stub linked as a static library; shell-script test harness diffs QEMU serial output.

**Tech Stack:** `x86_64-elf-gcc` (GCC 14, symlink), NASM 2.16, `x86_64-elf-ld` (binutils 2.44), Rust nightly (`x86_64-unknown-none`), QEMU 10, GNU Make, bash

**Spec:** `docs/superpowers/specs/2026-03-19-phase1-design.md`

---

## Files Created

| File | Purpose |
|------|---------|
| `tests/expected/boot.txt` | Ground truth — the failing test |
| `tests/run_tests.sh` | Test harness: boots QEMU, diffs output |
| `tools/linker.ld` | Linker script: load at 0x100000, .multiboot first |
| `Makefile` | All build targets |
| `kernel/arch/x86_64/boot.asm` | Multiboot2 header, 32→64 long mode, entry |
| `kernel/arch/x86_64/serial.h` | Internal serial interface (arch-only) |
| `kernel/arch/x86_64/serial.c` | COM1 driver, port I/O |
| `kernel/arch/x86_64/vga.h` | Internal VGA interface (arch-only) |
| `kernel/arch/x86_64/vga.c` | VGA text mode 80×25 |
| `kernel/arch/x86_64/arch.h` | Arch-neutral boundary: arch_init, arch_debug_exit, output fns |
| `kernel/arch/x86_64/arch.c` | arch_init() calls serial_init + vga_init |
| `kernel/arch/x86_64/arch_exit.c` | arch_debug_exit() — QEMU isa-debug-exit port I/O |
| `kernel/core/printk.h` | Declares printk() |
| `kernel/core/printk.c` | Routes to serial + VGA via arch.h |
| `kernel/core/main.c` | kernel_main: init sequence + halt |
| `kernel/cap/cap.h` | C header declaring cap_init() |
| `kernel/cap/Cargo.toml` | Rust crate config (no_std, staticlib) |
| `kernel/cap/rust-toolchain.toml` | Pins nightly + x86_64-unknown-none |
| `kernel/cap/src/lib.rs` | cap_init() stub |

---

## Task 1: Git init + project skeleton

**Files:** Creates root `.gitignore`, initialises git, creates all required directories.

- [ ] **Step 1: Initialise git repository**

```bash
cd /home/dylan/Developer/aegis
git init
```

Expected: `Initialized empty Git repository in .../aegis/.git/`

- [ ] **Step 2: Create directory structure**

```bash
mkdir -p kernel/arch/x86_64 kernel/core kernel/cap/src \
         tests/expected tools docs/superpowers/specs docs/superpowers/plans
```

- [ ] **Step 3: Write .gitignore**

Create `/home/dylan/Developer/aegis/.gitignore`:
```
build/
kernel/cap/target/
/tmp/aegis_serial.txt
*.o
*.elf
```

- [ ] **Step 4: Commit**

```bash
git add .gitignore MISSION.md .claude/
git commit -m "chore: project skeleton and .gitignore"
```

---

## Task 2: Test harness — the failing test (TASK 1 per MISSION.md)

**Files:**
- Create: `tests/expected/boot.txt`
- Create: `tests/run_tests.sh`

The harness is written BEFORE any kernel code. `make test` must fail immediately
(no binary). This is the RED state.

- [ ] **Step 1: Write boot.txt**

Create `tests/expected/boot.txt` with EXACTLY these four lines and a trailing newline.
No trailing spaces. No extra blank lines.

```
[SERIAL] OK: COM1 initialized at 115200 baud
[VGA] OK: text mode 80x25
[CAP] OK: capability subsystem reserved
[AEGIS] System halted.
```

Verify character count: `cat -A tests/expected/boot.txt` — each line ends with `$` (no spaces before newline).

- [ ] **Step 2: Write run_tests.sh**

Create `tests/run_tests.sh`:
```bash
#!/usr/bin/env bash
set -e

KERNEL=build/aegis.elf
EXPECTED=tests/expected/boot.txt
ACTUAL=/tmp/aegis_serial.txt

# Boot headless. QEMU exits with code 3 when kernel writes 0x01 to
# port 0xf4 (isa-debug-exit: exit_code = (value << 1) | 1 = 3).
# timeout prevents make test from hanging if kernel never signals exit.
timeout 10s qemu-system-x86_64 \
    -kernel "$KERNEL" \
    -nographic \
    -nodefaults \
    -serial stdio \
    -no-reboot \
    -m 128M \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    > "$ACTUAL" 2>/dev/null || true
# || true: QEMU exits 3 on clean kernel exit; set -e would treat that as failure.
# -nodefaults: prevents QEMU from adding default devices that may emit firmware
#              messages to the serial console, contaminating the diff.

diff "$EXPECTED" "$ACTUAL"
# diff exits 0 on match, 1 on mismatch. Expected first: missing lines show as -,
# unexpected lines as +.
```

- [ ] **Step 3: Make run_tests.sh executable**

```bash
chmod +x tests/run_tests.sh
```

- [ ] **Step 4: Verify the harness fails correctly (no kernel yet)**

```bash
bash tests/run_tests.sh
```

Expected: script fails because `build/aegis.elf` does not exist. The error will be
from QEMU (`could not load kernel 'build/aegis.elf'`) or the diff step. Either way,
exit code is non-zero. This is correct — the test is RED.

- [ ] **Step 5: Commit**

```bash
git add tests/
git commit -m "test: add test harness and expected output (RED)"
```

---

## Task 3: Linker script

**Files:**
- Create: `tools/linker.ld`

- [ ] **Step 1: Write linker script**

Create `tools/linker.ld`:
```
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

BASE = 0x100000;

SECTIONS
{
    . = BASE;

    /* .multiboot must land in the first 8KB so QEMU's -kernel scan finds it */
    .multiboot : { KEEP(*(.multiboot)) }

    .text   : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    }
}
```

- [ ] **Step 2: Verify syntax**

```bash
x86_64-elf-ld --verbose -T tools/linker.ld 2>&1 | head -5
```

Expected: prints linker script content or "no input files" error — either way
confirms the script was parsed without syntax errors. "no input files" is not a
script error.

- [ ] **Step 3: Commit**

```bash
git add tools/linker.ld
git commit -m "build: add linker script (identity-mapped at 0x100000)"
```

---

## Task 4: Makefile

**Files:**
- Create: `Makefile`

- [ ] **Step 1: Write Makefile**

Create `Makefile` at the repo root:
```makefile
CC    = x86_64-elf-gcc
AS    = nasm
LD    = x86_64-elf-ld
CARGO = cargo +nightly

# -nostdinc: exclude ALL system includes (kernel provides its own or uses none)
# -isystem ...: add back only GCC's own freestanding headers (stdint.h, stddef.h)
# -mno-red-zone: required for x86_64 kernels (interrupt handlers use stack below RSP)
# -mno-mmx/sse/sse2: no floating point or SIMD in kernel
# -fno-stack-protector: no canary (no runtime support for it)
# -Wall -Wextra -Werror: warnings are errors
GCC_INCLUDE := $(shell $(CC) -print-file-name=include)
CFLAGS = \
    -ffreestanding -nostdlib -nostdinc \
    -isystem $(GCC_INCLUDE) \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -fno-stack-protector \
    -Wall -Wextra -Werror \
    -Ikernel/arch/x86_64 \
    -Ikernel/core \
    -Ikernel/cap

ASFLAGS = -f elf64
LDFLAGS = -T tools/linker.ld -nostdlib

BUILD = build

ARCH_SRCS = \
    kernel/arch/x86_64/arch.c \
    kernel/arch/x86_64/arch_exit.c \
    kernel/arch/x86_64/serial.c \
    kernel/arch/x86_64/vga.c

CORE_SRCS = \
    kernel/core/main.c \
    kernel/core/printk.c

BOOT_SRC = kernel/arch/x86_64/boot.asm
CAP_LIB  = kernel/cap/target/x86_64-unknown-none/release/libcap.a

ARCH_OBJS = $(patsubst kernel/%.c,$(BUILD)/%.o,$(ARCH_SRCS))
CORE_OBJS = $(patsubst kernel/%.c,$(BUILD)/%.o,$(CORE_SRCS))
BOOT_OBJ  = $(BUILD)/arch/x86_64/boot.o

ALL_OBJS = $(BOOT_OBJ) $(ARCH_OBJS) $(CORE_OBJS)

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

- [ ] **Step 2: Verify Makefile is parseable**

```bash
make --dry-run 2>&1 | head -10
```

Expected: prints commands that would run, or "No rule to make target" for missing
source files. Should NOT be a Makefile syntax error.

- [ ] **Step 3: Commit**

```bash
git add Makefile
git commit -m "build: add Makefile with all targets"
```

---

## Task 5: Boot assembly (`boot.asm`)

**Files:**
- Create: `kernel/arch/x86_64/boot.asm`

This is the most complex file. CPU enters in 32-bit protected mode. We must
transition to 64-bit long mode before calling the C `kernel_main`.

- [ ] **Step 1: Write boot.asm**

Create `kernel/arch/x86_64/boot.asm`:
```nasm
; boot.asm — Multiboot2 header, 32-to-64 long mode setup, kernel entry
;
; Entry state per multiboot2 spec:
;   CPU: 32-bit protected mode, interrupts disabled, paging off
;   EAX: multiboot2 magic value (0x36D76289)
;   EBX: physical address of multiboot2 info structure
;   Stack: undefined — we set it up before any C call
;
; This file is x86-specific and intentionally lives in kernel/arch/x86_64/.

MULTIBOOT2_MAGIC  equ 0xE85250D6   ; identifies this as a multiboot2 header
MULTIBOOT2_ARCH   equ 0             ; 0 = i386 (32-bit protected mode entry)
KERNEL_STACK_SIZE equ 0x4000        ; 16KB boot stack

; ─── Multiboot2 header ────────────────────────────────────────────────────────
; Must land within the first 8KB of the ELF image so QEMU -kernel finds it.
; Linker script forces .multiboot to be the very first section.
section .multiboot
align 8
multiboot_header_start:
    dd MULTIBOOT2_MAGIC
    dd MULTIBOOT2_ARCH
    dd (multiboot_header_end - multiboot_header_start)
    ; Checksum: magic + arch + length + checksum must sum to 0 (mod 2^32)
    dd -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + \
         (multiboot_header_end - multiboot_header_start))
    ; End tag (type=0, flags=0, size=8)
    dw 0
    dw 0
    dd 8
multiboot_header_end:


; ─── GDT for 64-bit mode ──────────────────────────────────────────────────────
; Loaded in 32-bit mode before the long mode transition.
; Descriptor format: see Intel SDM Vol 3A, Section 3.4.5
section .data
align 8
gdt64:
    ; Entry 0: null descriptor (required)
    dq 0x0000000000000000
    ; Entry 1 (selector 0x08): 64-bit code segment
    ;   Limit: 0xFFFFF (ignored in 64-bit mode), Base: 0
    ;   Access: P=1, DPL=0, S=1, Type=1010 (code/execute/read)
    ;   Flags: G=1, L=1 (64-bit), D=0
    dq 0x00AF9A000000FFFF
    ; Entry 2 (selector 0x10): 64-bit data segment
    ;   Access: P=1, DPL=0, S=1, Type=0010 (data/read/write)
    ;   Flags: G=1, L=0 (data segments ignore L bit)
    dq 0x00AF92000000FFFF
gdt64_end:

; LGDT descriptor: 2-byte limit + 4-byte base (loaded in 32-bit mode)
gdt64_ptr:
    dw (gdt64_end - gdt64 - 1)
    dd gdt64


; ─── Entry point ──────────────────────────────────────────────────────────────
section .text
bits 32

; _start — first instruction executed after the bootloader hands off control.
;
; Purpose: transition from 32-bit protected mode to 64-bit long mode,
;          set up a stack, then call kernel_main.
; Clobbers: all registers
; Calling convention: none (we are the entry point)
global _start
_start:
    cli                         ; disable interrupts (undefined state from bootloader)

    ; Preserve multiboot2 args before we clobber EAX/EBX.
    ; mov edi,eax zero-extends to RDI (first System V AMD64 arg after far jump).
    ; mov esi,ebx zero-extends to RSI (second arg). Both values are < 4GB.
    mov edi, eax                ; mb_magic  → will be first  arg (RDI)
    mov esi, ebx                ; mb_info   → will be second arg (RSI)

    ; ── Load 64-bit GDT (still in 32-bit mode) ──────────────────────────
    lgdt [gdt64_ptr]

    ; ── Enable Physical Address Extension (required for long mode) ───────
    ; CR4.PAE = bit 5
    mov eax, cr4
    or  eax, (1 << 5)
    mov cr4, eax

    ; ── Build minimal identity-mapping page table ────────────────────────
    ; Layout: PML4 → PDPT → PD → 2MB huge page at physical address 0.
    ; This maps the first 2MB (which contains our kernel at 0x100000)
    ; so we can keep executing after enabling paging.
    ; All three tables are in .bss (zeroed by bootloader); we only write
    ; the entries we need and leave the rest as 0 (= not present).

    ; PML4[0] → PDPT  (present=1, writable=1 → flags 0x3)
    mov eax, pdpt_table
    or  eax, 0x3
    mov [pml4_table], eax

    ; PDPT[0] → PD  (present=1, writable=1)
    mov eax, pd_table
    or  eax, 0x3
    mov [pdpt_table], eax

    ; PD[0] → 2MB huge page at physical 0  (present=1, writable=1, PS=1 → 0x83)
    mov dword [pd_table], 0x83

    ; Load PML4 as the page-table root
    mov eax, pml4_table
    mov cr3, eax

    ; ── Set EFER.LME (Long Mode Enable) via MSR 0xC0000080 ───────────────
    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8)           ; EFER.LME
    wrmsr

    ; ── Enable paging (activates compatibility mode) ─────────────────────
    ; Also set CR0.WP (write protect) so ring-0 respects read-only pages.
    mov eax, cr0
    or  eax, (1 << 31) | (1 << 16)   ; CR0.PG | CR0.WP
    mov cr0, eax

    ; CPU is now in IA-32e compatibility submode.
    ; A far jump to a 64-bit code segment selector activates true 64-bit mode.

    ; ── Far jump: reload CS with 64-bit code descriptor ──────────────────
    ; Selector 0x08 = GDT entry 1 (64-bit code), RPL=0
    jmp 0x08:.long_mode_entry


bits 64
; .long_mode_entry — executes in true 64-bit long mode
;
; Purpose: set up segment registers and stack, then call kernel_main.
; Clobbers: AX, RSP (and anything kernel_main touches)
.long_mode_entry:
    ; ── Load 64-bit data segment into DS, ES, SS ─────────────────────────
    ; Selector 0x10 = GDT entry 2 (64-bit data). FS and GS unused in Phase 1.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; ── Set up the boot stack ─────────────────────────────────────────────
    ; boot_stack_top is the high end of a 16KB zero-initialised buffer in .bss.
    ; 16-byte aligned as required by the System V AMD64 ABI.
    mov rsp, boot_stack_top

    ; ── Call kernel_main(uint32_t mb_magic, void *mb_info) ───────────────
    ; RDI = mb_magic (set at _start, zero-extended from EAX)
    ; RSI = mb_info  (set at _start, zero-extended from EBX; physical addr < 4GB)
    ; Both are already in the correct registers from the moves at _start.
    extern kernel_main
    call kernel_main

    ; ── Hang if kernel_main returns (it must not) ─────────────────────────
.hang:
    hlt
    jmp .hang


; ─── BSS: page tables and boot stack ─────────────────────────────────────────
; The bootloader zeros .bss before calling _start.
; Page tables must be 4KB-aligned and zeroed (unused entries = not present).
section .bss
align 4096
pml4_table: resb 4096
pdpt_table: resb 4096
pd_table:   resb 4096

align 16
boot_stack:     resb KERNEL_STACK_SIZE
boot_stack_top:
```

- [ ] **Step 2: Verify it assembles**

```bash
nasm -f elf64 kernel/arch/x86_64/boot.asm -o /tmp/boot_test.o && echo "OK"
```

Expected: prints `OK`. Fix any NASM errors before continuing.

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/boot.asm
git commit -m "arch: add multiboot2 boot entry and 32-to-64 long mode setup"
```

---

## Task 6: Serial driver

**Files:**
- Create: `kernel/arch/x86_64/serial.h`
- Create: `kernel/arch/x86_64/serial.c`

COM1 on port 0x3F8. Polling only — no interrupts in Phase 1.

- [ ] **Step 1: Write serial.h**

Create `kernel/arch/x86_64/serial.h`:
```c
#ifndef SERIAL_H
#define SERIAL_H

/* Internal to kernel/arch/x86_64/ — not included from kernel/core/ */

/* Initialize COM1 at 115200 8N1. Prints [SERIAL] OK line on success.
 * Must be called before any other serial function. */
void serial_init(void);

/* Write a single character to COM1. Spins until transmit buffer empty. */
void serial_write_char(char c);

/* Write a null-terminated string to COM1. */
void serial_write_string(const char *s);

#endif
```

- [ ] **Step 2: Write serial.c**

Create `kernel/arch/x86_64/serial.c`:
```c
#include "serial.h"

/* COM1 base I/O port and register offsets */
#define COM1_BASE   0x3F8
#define COM1_DATA   (COM1_BASE + 0)   /* Data register (DLAB=0) / DLL (DLAB=1) */
#define COM1_IER    (COM1_BASE + 1)   /* Interrupt Enable (DLAB=0) / DLH (DLAB=1) */
#define COM1_FCR    (COM1_BASE + 2)   /* FIFO Control */
#define COM1_LCR    (COM1_BASE + 3)   /* Line Control */
#define COM1_MCR    (COM1_BASE + 4)   /* Modem Control */
#define COM1_LSR    (COM1_BASE + 5)   /* Line Status */

#define LSR_TXEMPTY (1 << 5)          /* Transmit-hold-register empty */

/* outb — write byte to I/O port.
 * Clobbers: none (volatile prevents reordering). */
static inline void outb(unsigned short port, unsigned char val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* inb — read byte from I/O port. */
static inline unsigned char inb(unsigned short port)
{
    unsigned char val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

void serial_init(void)
{
    outb(COM1_IER, 0x00);   /* disable all interrupts */
    outb(COM1_LCR, 0x80);   /* enable DLAB to set baud rate divisor */
    outb(COM1_DATA, 0x01);  /* divisor low byte: 1 → 115200 baud */
    outb(COM1_IER,  0x00);  /* divisor high byte: 0 */
    outb(COM1_LCR, 0x03);   /* 8 data bits, no parity, 1 stop bit; clear DLAB */
    outb(COM1_FCR, 0xC7);   /* enable FIFO, clear TX/RX, 14-byte threshold */
    outb(COM1_MCR, 0x0B);   /* assert DTR, RTS, OUT2 */

    serial_write_string("[SERIAL] OK: COM1 initialized at 115200 baud\n");
}

void serial_write_char(char c)
{
    /* Spin until transmit-hold register is empty */
    while ((inb(COM1_LSR) & LSR_TXEMPTY) == 0) {}
    outb(COM1_DATA, (unsigned char)c);
}

void serial_write_string(const char *s)
{
    while (*s != '\0') {
        serial_write_char(*s);
        s++;
    }
}
```

- [ ] **Step 3: Verify compilation**

```bash
x86_64-elf-gcc -ffreestanding -nostdlib -nostdinc \
    -isystem $(x86_64-elf-gcc -print-file-name=include) \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -fno-stack-protector -Wall -Wextra -Werror \
    -Ikernel/arch/x86_64 \
    -c kernel/arch/x86_64/serial.c -o /tmp/serial_test.o && echo "OK"
```

Expected: prints `OK`. Zero warnings (Werror would catch them).

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/serial.h kernel/arch/x86_64/serial.c
git commit -m "arch: add COM1 serial driver at 115200 baud (polling)"
```

---

## Task 7: VGA driver

**Files:**
- Create: `kernel/arch/x86_64/vga.h`
- Create: `kernel/arch/x86_64/vga.c`

Mode 3 text mode, 80×25, framebuffer at 0xB8000. ROM font, no file needed.

- [ ] **Step 1: Write vga.h**

Create `kernel/arch/x86_64/vga.h`:
```c
#ifndef VGA_H
#define VGA_H

/* Internal to kernel/arch/x86_64/ — not included from kernel/core/.
 * vga_available is declared in arch.h for use by printk. */

/* Set to 1 by vga_init() on success. Checked by printk via arch.h. */
extern int vga_available;

/* Initialize VGA text mode 80x25. Clears screen, sets vga_available=1,
 * prints [VGA] OK line. Must be called after serial_init(). */
void vga_init(void);

/* Write a single character to the VGA text buffer. Handles \n. */
void vga_write_char(char c);

/* Write a null-terminated string to the VGA text buffer. */
void vga_write_string(const char *s);

#endif
```

- [ ] **Step 2: Write vga.c**

Create `kernel/arch/x86_64/vga.c`:
```c
#include "vga.h"
#include "serial.h"   /* for vga_write_string fallback to serial in vga_init */

#define VGA_BASE    ((unsigned short *)0xB8000)
#define VGA_COLS    80
#define VGA_ROWS    25
#define VGA_ATTR    0x07    /* light grey on black */

int vga_available = 0;

static int vga_col = 0;
static int vga_row = 0;

/* vga_cell — encode a character + attribute as a 16-bit VGA cell. */
static inline unsigned short vga_cell(char c, unsigned char attr)
{
    return (unsigned short)((unsigned char)c) | ((unsigned short)attr << 8);
}

/* vga_scroll — shift all rows up by one, clear the bottom row. */
static void vga_scroll(void)
{
    int i;
    /* Move rows 1..24 up to rows 0..23 */
    for (i = 0; i < (VGA_ROWS - 1) * VGA_COLS; i++) {
        VGA_BASE[i] = VGA_BASE[i + VGA_COLS];
    }
    /* Clear bottom row */
    for (i = (VGA_ROWS - 1) * VGA_COLS; i < VGA_ROWS * VGA_COLS; i++) {
        VGA_BASE[i] = vga_cell(' ', VGA_ATTR);
    }
    vga_row = VGA_ROWS - 1;
}

void vga_init(void)
{
    int i;
    /* Clear entire screen */
    for (i = 0; i < VGA_ROWS * VGA_COLS; i++) {
        VGA_BASE[i] = vga_cell(' ', VGA_ATTR);
    }
    vga_col = 0;
    vga_row = 0;
    vga_available = 1;

    /* Print directly — printk is not yet up at this point */
    vga_write_string("[VGA] OK: text mode 80x25\n");
    serial_write_string("[VGA] OK: text mode 80x25\n");
}

void vga_write_char(char c)
{
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
        if (vga_row >= VGA_ROWS) {
            vga_scroll();
        }
        return;
    }

    VGA_BASE[vga_row * VGA_COLS + vga_col] = vga_cell(c, VGA_ATTR);
    vga_col++;
    if (vga_col >= VGA_COLS) {
        vga_col = 0;
        vga_row++;
        if (vga_row >= VGA_ROWS) {
            vga_scroll();
        }
    }
}

void vga_write_string(const char *s)
{
    while (*s != '\0') {
        vga_write_char(*s);
        s++;
    }
}
```

**Important:** `vga_init()` calls both `vga_write_string` (for the VGA display) AND
`serial_write_string` (so serial captures the VGA OK line). The output order in
`kernel_main` is `serial_init()` then `vga_init()`. The serial line from `serial_init`
appears first, then `vga_init` emits the VGA line to both outputs.

- [ ] **Step 3: Verify compilation**

```bash
x86_64-elf-gcc -ffreestanding -nostdlib -nostdinc \
    -isystem $(x86_64-elf-gcc -print-file-name=include) \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -fno-stack-protector -Wall -Wextra -Werror \
    -Ikernel/arch/x86_64 \
    -c kernel/arch/x86_64/vga.c -o /tmp/vga_test.o && echo "OK"
```

Expected: `OK`

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/vga.h kernel/arch/x86_64/vga.c
git commit -m "arch: add VGA text mode driver (80x25, ROM font)"
```

---

## Task 8: Arch interface (`arch.h`, `arch.c`, `arch_exit.c`)

**Files:**
- Create: `kernel/arch/x86_64/arch.h`
- Create: `kernel/arch/x86_64/arch.c`
- Create: `kernel/arch/x86_64/arch_exit.c`

`arch.h` is the ONLY arch header that `kernel/core/` ever includes. It exposes the
full arch-to-core boundary: init, exit, and the output functions used by `printk`.

- [ ] **Step 1: Write arch.h**

Create `kernel/arch/x86_64/arch.h`:
```c
#ifndef ARCH_H
#define ARCH_H

#include <stdint.h>   /* uint32_t etc. from GCC freestanding headers */

/*
 * arch.h — Architecture-neutral boundary for kernel/core/.
 *
 * Every architecture provides its own arch.h with this same interface.
 * The build system selects the right one via -Ikernel/arch/<target>.
 * kernel/core/ includes only "arch.h" — never any arch-specific header by name.
 *
 * Design debt (Phase 2): the output function declarations below
 * (serial_write_string, vga_write_string, vga_available) are x86-specific
 * in semantics and should migrate to a kernel/core/output.h abstraction
 * when an ARM64 port begins.
 */

/* Initialize all arch-specific subsystems (serial, VGA).
 * Must be the first call in kernel_main. */
void arch_init(void);

/* Signal QEMU isa-debug-exit device. QEMU exits with code (value << 1) | 1.
 * Writing 0x01 → QEMU exit code 3. No-op if device is absent. */
void arch_debug_exit(unsigned char value);

/*
 * Output primitives used by printk (implemented in serial.c / vga.c).
 * Declared here so kernel/core/printk.c can reach them via arch.h
 * without including serial.h or vga.h directly.
 */
extern int vga_available;              /* set to 1 by vga_init() */
void serial_write_string(const char *s);
void vga_write_string(const char *s);

#endif
```

- [ ] **Step 2: Write arch.c**

Create `kernel/arch/x86_64/arch.c`:
```c
#include "arch.h"
#include "serial.h"
#include "vga.h"

/*
 * arch_init — initialize all x86_64 early subsystems.
 *
 * Called once from kernel_main before any other subsystem.
 * Order matters: serial must be up before vga_init (vga_init calls serial).
 * Clobbers: nothing directly (subsystems manage their own state).
 */
void arch_init(void)
{
    serial_init();
    vga_init();
}
```

- [ ] **Step 3: Write arch_exit.c**

Create `kernel/arch/x86_64/arch_exit.c`:
```c
#include "arch.h"

/*
 * arch_debug_exit — signal QEMU's isa-debug-exit device.
 *
 * Writes `value` to I/O port 0xf4. QEMU translates this to process
 * exit code (value << 1) | 1. Writing 0x01 → exit code 3.
 * Only valid when QEMU is launched with:
 *   -device isa-debug-exit,iobase=0xf4,iosize=0x04
 * On real hardware this port is unassigned; the outb is a no-op.
 *
 * Clobbers: none.
 */
void arch_debug_exit(unsigned char value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"((unsigned short)0xf4));
}
```

- [ ] **Step 4: Verify compilation**

```bash
x86_64-elf-gcc -ffreestanding -nostdlib -nostdinc \
    -isystem $(x86_64-elf-gcc -print-file-name=include) \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -fno-stack-protector -Wall -Wextra -Werror \
    -Ikernel/arch/x86_64 \
    -c kernel/arch/x86_64/arch.c -o /tmp/arch_test.o && echo "arch OK"

x86_64-elf-gcc -ffreestanding -nostdlib -nostdinc \
    -isystem $(x86_64-elf-gcc -print-file-name=include) \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -fno-stack-protector -Wall -Wextra -Werror \
    -Ikernel/arch/x86_64 \
    -c kernel/arch/x86_64/arch_exit.c -o /tmp/arch_exit_test.o && echo "arch_exit OK"
```

Expected: both print `OK`.

- [ ] **Step 5: Commit**

```bash
git add kernel/arch/x86_64/arch.h kernel/arch/x86_64/arch.c \
        kernel/arch/x86_64/arch_exit.c
git commit -m "arch: add arch interface (arch_init, arch_debug_exit, output boundary)"
```

---

## Task 9: `printk`

**Files:**
- Create: `kernel/core/printk.h`
- Create: `kernel/core/printk.c`

Arch-agnostic. String-only. Serial always; VGA only if `vga_available`.

- [ ] **Step 1: Write printk.h**

Create `kernel/core/printk.h`:
```c
#ifndef PRINTK_H
#define PRINTK_H

/* Write a null-terminated string to all available outputs.
 * Routes to serial (always) and VGA (if initialised).
 * Not safe to call before arch_init(). */
void printk(const char *s);

#endif
```

- [ ] **Step 2: Write printk.c**

Create `kernel/core/printk.c`:
```c
#include "arch.h"
#include "printk.h"

/*
 * printk — route a string to all available output sinks.
 *
 * Law 1: serial is written unconditionally (if serial_init was called).
 * Law 2: VGA is written only if vga_available is set by vga_init().
 *        VGA failure never silences serial. Serial failure = kernel is blind.
 *
 * No format string support in Phase 1. Add when needed.
 */
void printk(const char *s)
{
    serial_write_string(s);
    if (vga_available) {
        vga_write_string(s);
    }
}
```

- [ ] **Step 3: Verify compilation**

```bash
x86_64-elf-gcc -ffreestanding -nostdlib -nostdinc \
    -isystem $(x86_64-elf-gcc -print-file-name=include) \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -fno-stack-protector -Wall -Wextra -Werror \
    -Ikernel/arch/x86_64 -Ikernel/core \
    -c kernel/core/printk.c -o /tmp/printk_test.o && echo "OK"
```

Expected: `OK`

- [ ] **Step 4: Commit**

```bash
git add kernel/core/printk.h kernel/core/printk.c
git commit -m "core: add printk (serial-first, VGA-conditional)"
```

---

## Task 10: CAP stub (Rust)

**Files:**
- Create: `kernel/cap/rust-toolchain.toml`
- Create: `kernel/cap/Cargo.toml`
- Create: `kernel/cap/cap.h`
- Create: `kernel/cap/src/lib.rs`

Phase 1 stub only. `cap_init()` prints its OK line and returns.

- [ ] **Step 1: Write rust-toolchain.toml**

Create `kernel/cap/rust-toolchain.toml`:
```toml
[toolchain]
channel = "nightly"
targets = ["x86_64-unknown-none"]
```

- [ ] **Step 2: Write Cargo.toml**

Create `kernel/cap/Cargo.toml`:
```toml
[package]
name = "cap"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["staticlib"]

[profile.dev]
panic = "abort"     # no unwinding infrastructure in a no_std kernel

[profile.release]
panic = "abort"
```

- [ ] **Step 3: Write cap.h**

Create `kernel/cap/cap.h`:
```c
#ifndef CAP_H
#define CAP_H

/* Initialize the capability subsystem.
 * Phase 1: stub only — prints status line and returns.
 * Implemented in Rust (kernel/cap/src/lib.rs), linked as libcap.a. */
void cap_init(void);

#endif
```

- [ ] **Step 4: Write lib.rs**

Create `kernel/cap/src/lib.rs`:
```rust
#![no_std]

extern "C" {
    // C declaration: void serial_write_string(const char *s)
    // On x86-64 with GCC, `char` is signed 8-bit and `u8` is unsigned 8-bit.
    // Both are 1 byte with identical ABI representation — safe to call with
    // a Rust byte-string literal (*const u8) on this target.
    fn serial_write_string(s: *const u8);
}

/// Initialize the capability subsystem.
///
/// Phase 1: stub only. Prints status line and returns immediately.
///
/// Note: writes directly to serial rather than through printk because no
/// `printk` Rust FFI wrapper exists yet. CAP output therefore does not
/// appear on VGA in Phase 1. Revisit when a printk wrapper is designed
/// (post-PMM/VMM phase).
#[no_mangle]
pub extern "C" fn cap_init() {
    // SAFETY: serial_init() is called in arch_init() before cap_init() is
    // called in kernel_main, so the serial port is fully initialized.
    // serial_write_string is a simple polling write with no shared mutable
    // state and no re-entrancy concerns at this point in boot.
    // The pointer is to a valid null-terminated byte literal in read-only data.
    // `char` and `u8` have identical 8-bit ABI representation on x86-64/GCC.
    unsafe {
        serial_write_string(b"[CAP] OK: capability subsystem reserved\n\0".as_ptr());
    }
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
```

- [ ] **Step 5: Build the Rust crate**

```bash
cargo +nightly build --release \
    --target x86_64-unknown-none \
    --manifest-path kernel/cap/Cargo.toml
```

Expected: compiles with zero errors. Output: `kernel/cap/target/x86_64-unknown-none/release/libcap.a`

Verify the archive was produced:
```bash
ls -lh kernel/cap/target/x86_64-unknown-none/release/libcap.a
```

- [ ] **Step 6: Commit**

```bash
git add kernel/cap/
git commit -m "cap: add no_std Rust capability stub (Phase 1: stub only)"
```

---

## Task 11: `kernel_main` + full build + `make test` GREEN

**Files:**
- Create: `kernel/core/main.c`

This is the integration task. After writing `main.c`, `make` should produce
`build/aegis.elf` and `make test` should exit 0.

- [ ] **Step 1: Write main.c**

Create `kernel/core/main.c`:
```c
#include "arch.h"
#include "printk.h"
#include "cap.h"

/*
 * kernel_main — top-level kernel entry point.
 *
 * Called from boot.asm after long mode is established and a stack is set up.
 * Arguments follow the System V AMD64 ABI (set up in boot.asm).
 *
 * mb_magic: multiboot2 magic value (0x36D76289). Ignored in Phase 1;
 *           validated in PMM phase when we parse the memory map.
 *
 * mb_info:  physical address of the multiboot2 info structure.
 *           IMPORTANT: this is a physical address, not a virtual pointer.
 *           It equals the virtual address in Phase 1 due to identity mapping.
 *           The VMM phase must remap this before dereferencing it post-paging.
 */
void kernel_main(uint32_t mb_magic, void *mb_info)
{
    (void)mb_magic;
    (void)mb_info;

    arch_init();                           /* serial_init + vga_init */
    cap_init();                            /* [CAP] OK line */
    printk("[AEGIS] System halted.\n");
    arch_debug_exit(0x01);                 /* QEMU exits with code 3 */
    for (;;) {}                            /* if not running in QEMU */
}
```

- [ ] **Step 2: Run `make` — kernel should link successfully**

```bash
make
```

Expected: compiles all C files, assembles boot.asm, links `build/aegis.elf`.
No warnings (Werror is set). Fix any errors before continuing.

Verify the ELF was produced and is a 64-bit binary:
```bash
file build/aegis.elf
```

Expected output contains: `ELF 64-bit LSB executable, x86-64`

- [ ] **Step 3: Quick sanity — run interactively and read serial output**

```bash
make run
```

Expected: QEMU starts, serial output appears in the terminal:
```
[SERIAL] OK: COM1 initialized at 115200 baud
[VGA] OK: text mode 80x25
[CAP] OK: capability subsystem reserved
[AEGIS] System halted.
```

QEMU exits immediately after the last line (via `arch_debug_exit`).
Press Ctrl+C if it doesn't exit (indicates `isa-debug-exit` device working).

- [ ] **Step 4: Run `make test` — this is the GREEN state**

```bash
make test
echo "Exit code: $?"
```

Expected:
```
Exit code: 0
```

If exit code is non-zero, `diff` will show which lines differ. Common issues:
- Extra QEMU startup messages in serial output → check `2>/dev/null` in run_tests.sh
- Trailing newline mismatch → verify boot.txt ends with exactly one newline
- Wrong output order → check `kernel_main` call sequence

- [ ] **Step 5: Commit**

```bash
git add kernel/core/main.c
git commit -m "core: add kernel_main, wire all subsystems — make test GREEN"
```

---

## Verification Checklist

After Task 11, confirm the following before declaring Phase 1 complete:

- [ ] `make test` exits 0
- [ ] `make clean && make test` exits 0 (clean build works)
- [ ] `make run` shows the four expected lines and exits
- [ ] No warnings or errors from `-Wall -Wextra -Werror`
- [ ] No x86-specific code exists in `kernel/core/` (only arch-neutral includes)
- [ ] `grep -r "0xB8000\|0x3F8\|outb\|inb" kernel/core/` returns nothing

---

## Troubleshooting

**`make test` hangs:** The kernel never called `arch_debug_exit`. Check that
`arch_debug_exit(0x01)` is reached in `kernel_main`. The `timeout 10s` in
`run_tests.sh` will kill QEMU after 10 seconds and the diff will fail.

**`diff` shows extra lines:** QEMU may be printing firmware messages to the serial
console. The `2>/dev/null` in `run_tests.sh` captures only stdout (COM1 via
`-serial stdio`). If firmware messages appear, try adding `-nodefaults` to the
QEMU flags in `run_tests.sh`.

**`boot.asm` assembles but kernel triple-faults:** Most likely the GDT or page
table setup is wrong. Run `make run` without `-nographic` (i.e., `make run`) to
see if QEMU's VGA output gives any clues. Add `-d cpu_reset` to the QEMU run
flags to see CPU state at reset.

**Rust link errors:** Ensure the `rust-toolchain.toml` is present in `kernel/cap/`
and that `cargo +nightly` resolves to the installed nightly. Run
`rustup toolchain list` to verify.
