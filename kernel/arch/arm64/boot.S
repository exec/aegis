/*
 * boot.S — ARM64 kernel entry (TTBR1 higher-half boot).
 *
 * QEMU loads at physical 0x40000000. _start runs with MMU off.
 * We build two page tables:
 *   TTBR0: identity map (VA=PA) for the boot transition
 *   TTBR1: kernel at 0xFFFF000040000000 → PA 0x40000000
 * Enable MMU, jump to high VA, call kernel_main.
 */

/* VA offset: high VA - physical addr */
#define VA_OFF 0xFFFF000000000000

    .section .text.boot
    .globl _start
_start:
    mov     x19, x0             /* save DTB pointer */
    msr     daifset, #0xf       /* mask all exceptions */
    msr     spsel, #1           /* use SP_EL1 */

    /* Enable FP/SIMD */
    mov     x1, #(3 << 20)
    msr     cpacr_el1, x1
    isb

    /* Zero BSS (physical addresses — MMU off).
     * __bss_start/__bss_end are high VAs; subtract VA_OFF to get PA.
     * Can't use adrp (±4GB range exceeded). Use ldr + subtract. */
    mov     x3, #0xFFFF
    lsl     x3, x3, #48         /* x3 = 0xFFFF000000000000 (VA offset) */
    ldr     x1, =__bss_start
    sub     x1, x1, x3          /* VA → PA */
    ldr     x2, =__bss_end
    sub     x2, x2, x3
.Lzero_bss:
    cmp     x1, x2
    b.ge    .Lbss_done
    str     xzr, [x1], #8
    b       .Lzero_bss
.Lbss_done:

    /* Boot stack (physical) */
    ldr     x1, =boot_stack_top
    sub     x1, x1, x3          /* VA to PA */
    mov     sp, x1

    /* Build page tables inline (can't call C — it's linked at high VA).
     * x3 = VA_OFF = 0xFFFF000000000000 (already set above).
     *
     * TTBR0 identity map: boot_l0[0] → boot_l1
     *   boot_l1[0] → device 1GB, boot_l1[1] → RAM 1GB
     * TTBR1 kernel map: kern_l0[0] → kern_l1
     *   kern_l1[0] → device 1GB, kern_l1[1] → RAM 1GB */

    /* Load physical addresses of page tables */
    ldr     x10, =boot_l0
    sub     x10, x10, x3        /* boot_l0 PA */
    ldr     x11, =boot_l1
    sub     x11, x11, x3        /* boot_l1 PA */
    ldr     x12, =kern_l0
    sub     x12, x12, x3        /* kern_l0 PA */
    ldr     x13, =kern_l1
    sub     x13, x13, x3        /* kern_l1 PA */

    /* Zero all four tables (512 × 8 = 4096 bytes each) */
    mov     x14, #512
    mov     x15, x10
1:  str     xzr, [x15], #8
    subs    x14, x14, #1
    b.ne    1b
    mov     x14, #512
    mov     x15, x11
2:  str     xzr, [x15], #8
    subs    x14, x14, #1
    b.ne    2b
    mov     x14, #512
    mov     x15, x12
3:  str     xzr, [x15], #8
    subs    x14, x14, #1
    b.ne    3b
    mov     x14, #512
    mov     x15, x13
4:  str     xzr, [x15], #8
    subs    x14, x14, #1
    b.ne    4b

    /* PTE constants */
    .set PTE_V, (1 << 0)       /* Valid */
    .set PTE_T, (1 << 1)       /* Table descriptor */
    .set PTE_AF, (1 << 10)     /* Access Flag */
    .set PTE_SH, (3 << 8)      /* Inner Shareable */
    .set PTE_DEV, (1 << 2)     /* AttrIndx=1 (Device) */

    /* TTBR0: boot_l0[0] → boot_l1 (table) */
    mov     x14, #(PTE_V | PTE_T)
    orr     x14, x14, x11      /* boot_l1 PA | valid | table */
    str     x14, [x10]          /* boot_l0[0] */

    /* boot_l1[0] = device 1GB block at PA 0 */
    mov     x14, #(PTE_V | PTE_AF | PTE_DEV)
    str     x14, [x11]          /* boot_l1[0] = 0 | device */

    /* boot_l1[1] = RAM 1GB block at PA 0x40000000 */
    mov     x14, #(PTE_V | PTE_AF | PTE_SH)
    orr     x14, x14, #0x40000000
    str     x14, [x11, #8]      /* boot_l1[1] */

    /* TTBR1: kern_l0[0] → kern_l1 (table) */
    mov     x14, #(PTE_V | PTE_T)
    orr     x14, x14, x13      /* kern_l1 PA */
    str     x14, [x12]          /* kern_l0[0] */

    /* kern_l1[0] = device 1GB at PA 0 */
    mov     x14, #(PTE_V | PTE_AF | PTE_DEV)
    str     x14, [x13]          /* kern_l1[0] */

    /* kern_l1[1] = RAM 1GB at PA 0x40000000 */
    mov     x14, #(PTE_V | PTE_AF | PTE_SH)
    orr     x14, x14, #0x40000000
    str     x14, [x13, #8]      /* kern_l1[1] */

    /* ── MMU configuration ── */

    /* MAIR: Attr0=Normal WB, Attr1=Device-nGnRnE */
    ldr     x1, =0x00000000000000FF
    msr     mair_el1, x1

    /* TCR: T0SZ=16 (48-bit user), T1SZ=16 (48-bit kernel),
     * TG0=4KB, TG1=4KB, Inner Shareable, WB Allocate */
    ldr     x1, =(16 | (0b11 << 12) | (0b01 << 10) | (0b01 << 8) | \
                  (16 << 16) | (0b10 << 30) | (0b11 << 28) | (0b01 << 26) | (0b01 << 24))
    msr     tcr_el1, x1

    /* TTBR0: identity map (boot_l0 PA in x10) */
    msr     ttbr0_el1, x10

    /* TTBR1: kernel map (kern_l0 PA in x12) */
    msr     ttbr1_el1, x12

    dsb     sy
    isb

    /* Enable MMU + caches */
    mrs     x1, sctlr_el1
    orr     x1, x1, #(1 << 0)
    orr     x1, x1, #(1 << 2)
    orr     x1, x1, #(1 << 12)
    msr     sctlr_el1, x1
    isb

    /* Jump to high VA (TTBR1 range) */
    ldr     x2, =.Lhigh
    br      x2

.Lhigh:
    /* Set up kernel stack */
    ldr     x1, =boot_stack_top
    mov     sp, x1

    /* Call kernel_main */
    mov     x0, x19
    ldr     x2, =kernel_main
    blr     x2

.Lhang:
    wfi
    b       .Lhang

    /* Boot stack */
    .section .bss
    .align 16
boot_stack_bottom:
    .space 16384
    .globl boot_stack_top
boot_stack_top:
