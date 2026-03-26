/*
 * vectors.S — ARM64 exception vector table + handlers.
 *
 * Vector layout: 16 entries × 128 bytes, 2KB aligned.
 *
 * On EL0→EL1 exception entry, we switch TTBR0 to the master (kernel) PML4
 * so device MMIO is accessible. Before ERET back to EL0, we restore the
 * user PML4. The user TTBR0 is saved in a global (single-CPU only).
 */

/* ── Macros ──────────────────────────────────────────────────────── */

/* No TTBR0 switch needed — user pages mapped in master PML4.
 * TTBR1-based kernel/user isolation is future work. */
.macro KERNEL_ENTRY_FROM_EL0
.endm

.macro KERNEL_EXIT_TO_EL0
.endm

.macro SAVE_ALL_EL0
    sub     sp, sp, #(34 * 8)
    stp     x0,  x1,  [sp, #(0*8)]
    stp     x2,  x3,  [sp, #(2*8)]
    stp     x4,  x5,  [sp, #(4*8)]
    stp     x6,  x7,  [sp, #(6*8)]
    stp     x8,  x9,  [sp, #(8*8)]
    stp     x10, x11, [sp, #(10*8)]
    stp     x12, x13, [sp, #(12*8)]
    stp     x14, x15, [sp, #(14*8)]
    stp     x16, x17, [sp, #(16*8)]
    stp     x18, x19, [sp, #(18*8)]
    stp     x20, x21, [sp, #(20*8)]
    stp     x22, x23, [sp, #(22*8)]
    stp     x24, x25, [sp, #(24*8)]
    stp     x26, x27, [sp, #(26*8)]
    stp     x28, x29, [sp, #(28*8)]
    str     x30,       [sp, #(30*8)]
    mrs     x10, sp_el0
    mrs     x11, elr_el1
    mrs     x12, spsr_el1
    stp     x10, x11, [sp, #(31*8)]
    str     x12,       [sp, #(33*8)]
.endm

.macro RESTORE_ALL_EL0
    ldp     x10, x11, [sp, #(31*8)]
    ldr     x12,       [sp, #(33*8)]
    msr     sp_el0,    x10
    msr     elr_el1,   x11
    /* Validate SPSR: force EL0t (M[3:0]=0b0000), clear reserved bits.
     * Prevents privilege escalation via crafted signal frames. */
    and     x12, x12, #0xF0000000   /* Keep only NZCV flags */
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
.endm

/* ── Global: saved user TTBR0 (single-CPU) ─────────────────────── */
    .section .bss
    .balign 8
    .globl g_saved_user_ttbr0
g_saved_user_ttbr0:
    .space 8

/* ── Vector table ──────────────────────────────────────────────── */
    .section .text
    .balign 2048
    .globl exception_vectors
exception_vectors:
/* Current EL, SP_EL0 (unused) */
    .balign 128
    b       _exc_hang
    .balign 128
    b       _exc_hang
    .balign 128
    b       _exc_hang
    .balign 128
    b       _exc_hang

/* Current EL, SP_ELx (kernel mode) */
    .balign 128
    b       _exc_sync_el1
    .balign 128
    b       _exc_irq_el1
    .balign 128
    b       _exc_hang
    .balign 128
    b       _exc_hang

/* Lower EL, AArch64 (user mode) */
    .balign 128
    b       _exc_sync_el0
    .balign 128
    b       _exc_irq_el0
    .balign 128
    b       _exc_hang
    .balign 128
    b       _exc_hang

/* Lower EL, AArch32 (unused) */
    .balign 128
    b       _exc_hang
    .balign 128
    b       _exc_hang
    .balign 128
    b       _exc_hang
    .balign 128
    b       _exc_hang

/* ── Handlers ──────────────────────────────────────────────────── */

_exc_hang:
    wfi
    b       _exc_hang

_exc_sync_el1:
    stp     x0, x1,   [sp, #-16]!
    stp     x2, x3,   [sp, #-16]!
    stp     x29, x30, [sp, #-16]!
    bl      exc_sync_handler
    ldp     x29, x30, [sp], #16
    ldp     x2, x3,   [sp], #16
    ldp     x0, x1,   [sp], #16
    eret

_exc_irq_el1:
    stp     x0, x1,   [sp, #-16]!
    stp     x2, x3,   [sp, #-16]!
    stp     x4, x5,   [sp, #-16]!
    stp     x6, x7,   [sp, #-16]!
    stp     x8, x9,   [sp, #-16]!
    stp     x10, x11, [sp, #-16]!
    stp     x12, x13, [sp, #-16]!
    stp     x14, x15, [sp, #-16]!
    stp     x16, x17, [sp, #-16]!
    stp     x18, x30, [sp, #-16]!
    stp     x29, xzr, [sp, #-16]!
    bl      irq_handler
    ldp     x29, xzr, [sp], #16
    ldp     x18, x30, [sp], #16
    ldp     x16, x17, [sp], #16
    ldp     x14, x15, [sp], #16
    ldp     x12, x13, [sp], #16
    ldp     x10, x11, [sp], #16
    ldp     x8, x9,   [sp], #16
    ldp     x6, x7,   [sp], #16
    ldp     x4, x5,   [sp], #16
    ldp     x2, x3,   [sp], #16
    ldp     x0, x1,   [sp], #16
    eret

/* ── EL0 Synchronous (SVC + faults) ── */
_exc_sync_el0:
    KERNEL_ENTRY_FROM_EL0
    SAVE_ALL_EL0

    mrs     x9, esr_el1
    lsr     x9, x9, #26
    cmp     x9, #0x15
    b.ne    .Lel0_not_svc

    /* syscall_dispatch(frame, num, a1..a6) */
    mov     x0, sp
    ldr     x1, [sp, #(8*8)]       /* x8 = syscall number */
    ldr     x2, [sp, #(0*8)]       /* x0 = arg1 */
    ldr     x3, [sp, #(1*8)]       /* x1 = arg2 */
    ldr     x4, [sp, #(2*8)]       /* x2 = arg3 */
    ldr     x5, [sp, #(3*8)]       /* x3 = arg4 */
    ldr     x6, [sp, #(4*8)]       /* x4 = arg5 */
    ldr     x7, [sp, #(5*8)]       /* x5 = arg6 */
    bl      syscall_dispatch
    str     x0, [sp, #(0*8)]       /* store return value in saved x0 */
    b       .Lel0_return

.Lel0_not_svc:
    mov     x0, sp
    bl      exc_sync_el0_handler

.Lel0_return:
    RESTORE_ALL_EL0
    KERNEL_EXIT_TO_EL0
    eret

/* ── EL0 IRQ ── */
_exc_irq_el0:
    KERNEL_ENTRY_FROM_EL0

    /* Save TTBR0 before sched_tick may switch tasks.
     * After ctx_switch, we may return to a DIFFERENT task.
     * That task's TTBR0 must be loaded before ERET. */
    mrs     x9, ttbr0_el1
    sub     sp, sp, #16
    str     x9, [sp]

    SAVE_ALL_EL0
    bl      irq_handler
    RESTORE_ALL_EL0

    /* If sched_tick switched tasks, the current task may differ
     * from the one that entered. Load current task's TTBR0. */
    bl      fork_child_load_ttbr0

    /* Discard saved TTBR0 (we loaded the correct one above) */
    add     sp, sp, #16

    KERNEL_EXIT_TO_EL0
    eret
