OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHYS_BASE = 0x100000;
KERN_VMA  = 0xFFFFFFFF80000000;

SECTIONS
{
    . = PHYS_BASE;

    /* .multiboot must land within the first 8KB so GRUB locates it
       per the multiboot2 spec. VMA = LMA = physical. */
    .multiboot : { KEEP(*(.multiboot)) }

    /* .text.boot contains the 32-bit boot stub, GDT, and physical-mode
       trampoline. Must run at physical addresses — VMA = LMA. */
    .text.boot  : { *(.text.boot) }

    /* Relocate all remaining sections to the higher half.
       VMA = KERN_VMA + offset; LMA = physical (AT directive). */
    . += KERN_VMA;

    .text   : AT(ADDR(.text)   - KERN_VMA) { *(.text .text.*) }
    .rodata : AT(ADDR(.rodata) - KERN_VMA) { *(.rodata .rodata.*) }
    .data   : AT(ADDR(.data)   - KERN_VMA) { *(.data .data.*) }

    .bss : AT(ADDR(.bss) - KERN_VMA) {
        *(COMMON)
        *(.bss .bss.*)
    }

    _kernel_end = .;   /* higher-half virtual address */
}
