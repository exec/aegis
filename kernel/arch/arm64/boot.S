/*
 * boot.S — ARM64 kernel entry point for QEMU virt machine.
 *
 * QEMU -kernel loads this as an ELF or raw binary. On entry:
 *   x0 = physical address of device tree blob (DTB)
 *   x1 = 0 (reserved)
 *   x2 = 0 (reserved)
 *   x3 = 0 (reserved)
 *   CPU is in EL1 (QEMU virt drops to EL1 before entry)
 *   MMU is off, caches may be on or off
 *
 * This stub:
 *   1. Masks all exceptions (DAIF)
 *   2. Sets up a 16KB boot stack in BSS
 *   3. Jumps to kernel_main(dtb_phys) in C
 */

    .section .text.boot
    .globl _start
_start:
    /* Mask all exceptions (debug, SError, IRQ, FIQ) */
    msr     daifset, #0xf

    /* Zero BSS — linker defines __bss_start and __bss_end */
    adrp    x1, __bss_start
    add     x1, x1, :lo12:__bss_start
    adrp    x2, __bss_end
    add     x2, x2, :lo12:__bss_end
.Lzero_bss:
    cmp     x1, x2
    b.ge    .Lbss_done
    str     xzr, [x1], #8
    b       .Lzero_bss
.Lbss_done:

    /* Set up boot stack (grows downward — SP points to top) */
    adrp    x1, boot_stack_top
    add     x1, x1, :lo12:boot_stack_top
    mov     sp, x1

    /* x0 still holds DTB physical address from firmware */
    bl      kernel_main

    /* kernel_main should not return; halt if it does */
.Lhang:
    wfi
    b       .Lhang

    /* 16KB boot stack in BSS */
    .section .bss
    .align 16
boot_stack_bottom:
    .space  16384
boot_stack_top:
