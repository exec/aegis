/*
 * boot.S — ARM64 kernel entry point for QEMU virt machine.
 *
 * QEMU -kernel loads this as an ELF. On entry:
 *   x0 = physical address of device tree blob (DTB)
 *   CPU is in EL1 (QEMU virt drops to EL1 before entry)
 *   MMU is off, caches may be on or off
 *
 * This stub:
 *   1. Masks all exceptions (DAIF)
 *   2. Zeros BSS
 *   3. Sets up a 16KB boot stack
 *   4. Calls mmu_early_init() to build identity-map page tables
 *   5. Configures MAIR, TCR, TTBR0 and enables the MMU
 *   6. Jumps to kernel_main(dtb_phys) in C
 */

    .section .text.boot
    .globl _start
_start:
    /* Save DTB pointer — x0 is clobbered by BSS zeroing */
    mov     x19, x0

    /* Mask all exceptions (debug, SError, IRQ, FIQ) */
    msr     daifset, #0xf

    /* Zero BSS */
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

    /* Set up boot stack */
    adrp    x1, boot_stack_top
    add     x1, x1, :lo12:boot_stack_top
    mov     sp, x1

    /* Build early identity-map page tables */
    bl      mmu_early_init

    /* ── Configure MMU registers ── */

    /* MAIR_EL1: Memory Attribute Indirection Register
     *   Attr0 = 0xFF → Normal, Write-Back, Read/Write Allocate (inner+outer)
     *   Attr1 = 0x00 → Device-nGnRnE (strongly ordered, no gather, no reorder) */
    ldr     x1, =0x00000000000000FF
    msr     mair_el1, x1

    /* TCR_EL1: Translation Control Register
     *   T0SZ  = 16  → 48-bit VA for TTBR0 (bits [5:0])
     *   TG0   = 0b00 → 4KB granule for TTBR0 (bits [15:14])
     *   SH0   = 0b11 → Inner Shareable (bits [13:12])
     *   ORGN0 = 0b01 → Outer Write-Back Allocate (bits [11:10])
     *   IRGN0 = 0b01 → Inner Write-Back Allocate (bits [9:8])
     *
     * Bits [31:16] control TTBR1 — left as 0 (unused for now). */
    ldr     x1, =(16 | (0b11 << 12) | (0b01 << 10) | (0b01 << 8))
    msr     tcr_el1, x1

    /* TTBR0_EL1: point at boot_l0 (built by mmu_early_init) */
    adrp    x1, boot_l0
    add     x1, x1, :lo12:boot_l0
    msr     ttbr0_el1, x1

    /* Ensure all table writes are visible before enabling MMU */
    dsb     sy
    isb

    /* Enable MMU: set SCTLR_EL1.M (bit 0), also C (bit 2) for data cache,
     * I (bit 12) for instruction cache. */
    mrs     x1, sctlr_el1
    orr     x1, x1, #(1 << 0)    /* M — MMU enable */
    orr     x1, x1, #(1 << 2)    /* C — data cache enable */
    orr     x1, x1, #(1 << 12)   /* I — instruction cache enable */
    msr     sctlr_el1, x1
    isb

    /* MMU is now on. Identity map means VA == PA — we continue executing. */

    /* Restore DTB pointer and call kernel_main */
    mov     x0, x19
    bl      kernel_main

    /* kernel_main should not return */
.Lhang:
    wfi
    b       .Lhang

    /* 16KB boot stack in BSS */
    .section .bss
    .align 16
boot_stack_bottom:
    .space  16384
boot_stack_top:
