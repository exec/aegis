/*
 * linker.ld — ARM64 kernel (TTBR1 higher-half).
 *
 * .text.boot: VMA=LMA=0x40000000 (runs before MMU, identity-mapped)
 * Everything else: VMA=0xFFFF000040xxxxxx, LMA=0x40xxxxxx
 * The constant offset VMA-LMA = 0xFFFF000000000000 matches TTBR1's
 * 1GB block mapping (kern_l1[1]: VA 0xFFFF000040000000 → PA 0x40000000).
 */

KERN_VMA = 0xFFFF000040000000;
KERN_LMA = 0x40000000;
VA_OFF   = KERN_VMA - KERN_LMA;    /* 0xFFFF000000000000 */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    /* Boot stub at physical address (identity-mapped via TTBR0) */
    . = KERN_LMA;
    .text.boot : {
        KEEP(*(.text.boot))
    }

    /* Align to page boundary, then switch to high VMA.
     * CRITICAL: LMA must equal VMA - VA_OFF for the 1GB block mapping. */
    . = ALIGN(4096);
    _post_boot = .;

    . = KERN_VMA + (_post_boot - KERN_LMA);

    .text : AT(ADDR(.text) - VA_OFF) {
        *(.text .text.*)
    }

    .rodata : AT(ADDR(.rodata) - VA_OFF) {
        *(.rodata .rodata.*)
    }

    .data : AT(ADDR(.data) - VA_OFF) {
        *(.data .data.*)
    }

    . = ALIGN(4096);
    __bss_start = .;
    .bss (NOLOAD) : AT(ADDR(.bss) - VA_OFF) {
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
