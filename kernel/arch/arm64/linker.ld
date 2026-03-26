/*
 * linker.ld — ARM64 kernel linker script for QEMU virt machine.
 *
 * QEMU -kernel loads the ELF at the entry point address.
 * The virt machine RAM starts at 0x40000000. We link at this address
 * with MMU off (physical == virtual at boot).
 */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text.boot : {
        KEEP(*(.text.boot))
    }

    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    . = ALIGN(8);
    __bss_start = .;
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN(8);
    __bss_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
    }
}
