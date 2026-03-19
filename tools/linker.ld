OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

BASE = 0x100000;

SECTIONS
{
    . = BASE;

    /* .multiboot must land within the first 8KB so the bootloader (GRUB)
       locates it per the multiboot2 spec */
    .multiboot : { KEEP(*(.multiboot)) }

    .text   : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    }
    _kernel_end = .;
}
