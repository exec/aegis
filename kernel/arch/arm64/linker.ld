/*
 * linker.ld — ARM64 kernel linker script (higher-half via TTBR1).
 *
 * VMA (virtual): 0xFFFF000040000000 — kernel runs at this address after
 * MMU is enabled with TTBR1 page tables.
 *
 * LMA (load/physical): 0x40000000 — QEMU loads the ELF here. The .text.boot
 * section runs at LMA (identity-mapped via TTBR0) until the jump to high VA.
 *
 * KERN_VMA - KERN_LMA = 0xFFFF000000000000 (the offset applied to all
 * kernel symbols).
 */

KERN_VMA = 0xFFFF000040000000;
KERN_LMA = 0x40000000;

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    /* Boot code runs at physical address (identity-mapped via TTBR0).
     * _start is at LMA so QEMU can jump to it before MMU is on. */
    . = KERN_LMA;
    .text.boot : {
        KEEP(*(.text.boot))
    }

    /* Everything else is linked at high VMA but loaded at LMA.
     * AT(addr) sets the LMA; the linker computes VMA from '.'. */
    . = KERN_VMA + SIZEOF(.text.boot);

    .text : AT(KERN_LMA + SIZEOF(.text.boot)) {
        *(.text .text.*)
    }

    .rodata : AT(LOADADDR(.text) + SIZEOF(.text)) {
        *(.rodata .rodata.*)
    }

    .data : AT(LOADADDR(.rodata) + SIZEOF(.rodata)) {
        *(.data .data.*)
    }

    . = ALIGN(4096);
    __bss_start = .;
    .bss : AT(LOADADDR(.data) + SIZEOF(.data)) {
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN(4096);
    __bss_end = .;
    _kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
    }
}
