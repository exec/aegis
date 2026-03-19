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
