/*
 * proc_enter.S — ARM64 EL1→EL0 transition trampolines.
 *
 * proc_enter_user:
 *   Called via ctx_switch ret (x30) for the first scheduling of init.
 *   Pops 4 values from stack: SPSR, ELR, user_SP, PML4_phys.
 *   Sets ELR/SPSR/SP_EL0, then ERETs to EL0.
 *   NOTE: TTBR0 is NOT switched — user pages are mapped in the master PML4.
 *   User address space isolation via TTBR1 is future work.
 *
 * fork_child_return:
 *   Called via ctx_switch ret (x30) for a fork child's first scheduling.
 *   Restores SAVE_ALL_EL0 frame and ERETs to EL0.
 */

    .section .text
    .globl proc_enter_user
    .globl fork_child_return

proc_enter_user:
    /* Stack has (low → high): SPSR, ELR, user_SP, PML4_phys */
    ldp     x0, x1, [sp], #16      /* x0 = SPSR, x1 = ELR */
    ldp     x2, x3, [sp], #16      /* x2 = user_SP, x3 = PML4_phys */

    /* Switch TTBR0 to user page tables */
    msr     ttbr0_el1, x3
    dsb     sy
    isb
    tlbi    vmalle1
    dsb     sy
    isb

    /* Set return state */
    msr     spsr_el1, x0
    msr     elr_el1, x1
    msr     sp_el0, x2

    /* Clear all GPRs so no kernel data leaks to user */
    mov     x0, #0
    mov     x1, #0
    mov     x2, #0
    mov     x3, #0
    mov     x4, #0
    mov     x5, #0
    mov     x6, #0
    mov     x7, #0
    mov     x8, #0
    mov     x9, #0
    mov     x10, #0
    mov     x11, #0
    mov     x12, #0
    mov     x13, #0
    mov     x14, #0
    mov     x15, #0
    mov     x16, #0
    mov     x17, #0
    mov     x18, #0
    mov     x19, #0
    mov     x20, #0
    mov     x21, #0
    mov     x22, #0
    mov     x23, #0
    mov     x24, #0
    mov     x25, #0
    mov     x26, #0
    mov     x27, #0
    mov     x28, #0
    mov     x29, #0
    mov     x30, #0

    eret

fork_child_return:
    /* Restore SAVE_ALL_EL0 frame and ERET */
    ldp     x10, x11, [sp, #(31*8)]
    ldr     x12,       [sp, #(33*8)]
    msr     sp_el0,    x10
    msr     elr_el1,   x11
    msr     spsr_el1,  x12
    ldp     x0,  x1,  [sp, #(0*8)]
    ldp     x2,  x3,  [sp, #(2*8)]
    ldp     x4,  x5,  [sp, #(4*8)]
    ldp     x6,  x7,  [sp, #(6*8)]
    ldp     x8,  x9,  [sp, #(8*8)]
    ldp     x10, x11, [sp, #(10*8)]
    ldp     x12, x13, [sp, #(12*8)]
    ldp     x14, x15, [sp, #(14*8)]
    ldp     x16, x17, [sp, #(16*8)]
    ldp     x18, x19, [sp, #(18*8)]
    ldp     x20, x21, [sp, #(20*8)]
    ldp     x22, x23, [sp, #(22*8)]
    ldp     x24, x25, [sp, #(24*8)]
    ldp     x26, x27, [sp, #(26*8)]
    ldp     x28, x29, [sp, #(28*8)]
    ldr     x30,       [sp, #(30*8)]
    add     sp, sp, #(34 * 8)
    eret
