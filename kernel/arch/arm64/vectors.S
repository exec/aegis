/*
 * vectors.S — ARM64 exception vector table.
 *
 * ARM64 exception vectors: 16 entries, 128 bytes each (32 instructions).
 * Layout:
 *   [0]  Current EL, SP_EL0, Synchronous
 *   [1]  Current EL, SP_EL0, IRQ
 *   [2]  Current EL, SP_EL0, FIQ
 *   [3]  Current EL, SP_EL0, SError
 *   [4]  Current EL, SP_ELx, Synchronous
 *   [5]  Current EL, SP_ELx, IRQ        ← kernel IRQ handler (timer)
 *   [6]  Current EL, SP_ELx, FIQ
 *   [7]  Current EL, SP_ELx, SError
 *   [8]  Lower EL, AArch64, Synchronous  ← syscall (SVC) from user
 *   [9]  Lower EL, AArch64, IRQ          ← user-mode IRQ
 *   [10] Lower EL, AArch64, FIQ
 *   [11] Lower EL, AArch64, SError
 *   [12-15] Lower EL, AArch32 (unused)
 *
 * For now, only vector [5] (kernel IRQ) is meaningful — the timer.
 * All others print a diagnostic and hang.
 */

    .section .text
    .balign 2048
    .globl exception_vectors
exception_vectors:

/* ── Current EL, SP_EL0 ── (not used, we use SP_ELx) */
    /* [0] Synchronous */
    .balign 128
    b       _exc_hang

    /* [1] IRQ */
    .balign 128
    b       _exc_hang

    /* [2] FIQ */
    .balign 128
    b       _exc_hang

    /* [3] SError */
    .balign 128
    b       _exc_hang

/* ── Current EL, SP_ELx ── (kernel mode exceptions) */
    /* [4] Synchronous */
    .balign 128
    b       _exc_sync_el1

    /* [5] IRQ — timer fires here */
    .balign 128
    b       _exc_irq_el1

    /* [6] FIQ */
    .balign 128
    b       _exc_hang

    /* [7] SError */
    .balign 128
    b       _exc_hang

/* ── Lower EL, AArch64 ── (user mode exceptions — future) */
    /* [8] Synchronous (SVC) */
    .balign 128
    b       _exc_hang

    /* [9] IRQ */
    .balign 128
    b       _exc_hang

    /* [10] FIQ */
    .balign 128
    b       _exc_hang

    /* [11] SError */
    .balign 128
    b       _exc_hang

/* ── Lower EL, AArch32 ── (unused) */
    .balign 128
    b       _exc_hang
    .balign 128
    b       _exc_hang
    .balign 128
    b       _exc_hang
    .balign 128
    b       _exc_hang

/* ── Handler stubs ─────────────────────────────────────────────── */

_exc_hang:
    wfi
    b       _exc_hang

/*
 * _exc_sync_el1 — kernel synchronous exception.
 * Read ESR_EL1 for cause. For now, just hang.
 */
_exc_sync_el1:
    /* Save caller-saved regs */
    stp     x0, x1,   [sp, #-16]!
    stp     x2, x3,   [sp, #-16]!
    stp     x29, x30, [sp, #-16]!

    bl      exc_sync_handler

    ldp     x29, x30, [sp], #16
    ldp     x2, x3,   [sp], #16
    ldp     x0, x1,   [sp], #16
    eret

/*
 * _exc_irq_el1 — kernel IRQ exception.
 * Save registers, call irq_handler (C), restore, eret.
 */
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
