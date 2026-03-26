/*
 * ctx_switch.S — ARM64 context switch.
 *
 * void ctx_switch(aegis_task_t *outgoing, aegis_task_t *incoming)
 *
 * AArch64 calling convention:
 *   x0 = outgoing task (first arg)
 *   x1 = incoming task (second arg)
 *
 * aegis_task_t.sp is at offset 0 (guaranteed by _Static_assert in sched.c).
 *
 * Callee-saved registers (must preserve across function calls):
 *   x19-x28, x29 (fp), x30 (lr)
 *
 * Strategy:
 *   1. Push callee-saved regs onto outgoing's stack
 *   2. Save outgoing's SP into outgoing->sp
 *   3. Load incoming->sp into SP
 *   4. Pop callee-saved regs from incoming's stack
 *   5. ret (jumps to incoming's saved LR)
 *
 * For a newly spawned task, sched_spawn sets up the stack to look like
 * ctx_switch already ran: 12 slots of zeros (x19-x28, fp) + fn address (lr).
 * ctx_switch pops these, ret jumps to fn.
 */

    .section .text
    .globl ctx_switch
ctx_switch:
    /* Save callee-saved registers on outgoing stack */
    stp     x19, x20, [sp, #-16]!
    stp     x21, x22, [sp, #-16]!
    stp     x23, x24, [sp, #-16]!
    stp     x25, x26, [sp, #-16]!
    stp     x27, x28, [sp, #-16]!
    stp     x29, x30, [sp, #-16]!    /* x29=fp, x30=lr (return address) */

    /* Save outgoing SP → outgoing->sp (offset 0) */
    mov     x9, sp
    str     x9, [x0]

    /* Load incoming->sp → SP */
    ldr     x9, [x1]
    mov     sp, x9

    /* Restore callee-saved registers from incoming stack */
    ldp     x29, x30, [sp], #16
    ldp     x27, x28, [sp], #16
    ldp     x25, x26, [sp], #16
    ldp     x23, x24, [sp], #16
    ldp     x21, x22, [sp], #16
    ldp     x19, x20, [sp], #16

    /* ret → jumps to x30 (lr), which is the incoming task's saved return addr */
    ret
