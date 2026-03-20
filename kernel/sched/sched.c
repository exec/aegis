#include "sched.h"
#include "arch.h"
#include "pmm.h"
#include "printk.h"
#include <stddef.h>

/* Compile-time guard: ctx_switch.asm assumes rsp is at offset 0 of TCB.
 * If anyone adds a field before rsp, this catches it immediately. */
_Static_assert(offsetof(aegis_task_t, rsp) == 0,
    "rsp must be first field in aegis_task_t — ctx_switch depends on this");

#define STACK_PAGES  4                     /* 16KB per task */
#define STACK_SIZE   (STACK_PAGES * 4096UL)

static aegis_task_t *s_current = (void *)0;
static uint32_t      s_next_tid = 0;
static uint32_t      s_task_count = 0;

void
sched_init(void)
{
    s_current    = (void *)0;
    s_next_tid   = 0;
    s_task_count = 0;
}

void
sched_spawn(void (*fn)(void))
{
    /* Allocate TCB (one page from PMM — plenty of space).
     *
     * IDENTITY MAP DEPENDENCY: pmm_alloc_page() returns a physical address.
     * The cast to aegis_task_t * is valid only while the identity window
     * [0..4MB) is active. Phase 4 must not tear down the identity map before
     * replacing these raw physical casts with a mapped-window allocator.
     * See CLAUDE.md "Phase 3 forward-looking constraints". */
    uint64_t tcb_phys = pmm_alloc_page();
    if (!tcb_phys) {
        printk("[SCHED] FAIL: OOM allocating TCB\n");
        for (;;) {}
    }
    aegis_task_t *task = (aegis_task_t *)(uintptr_t)tcb_phys;

    /* Allocate stack (STACK_PAGES individual pages).
     *
     * CONTIGUITY ASSUMPTION: The Phase 3 PMM is a bitmap allocator over the
     * physical memory map. Early boot memory is a single contiguous range and
     * the bitmap allocates sequentially, so successive pmm_alloc_page() calls
     * return physically adjacent frames. This allows treating the pages as a
     * single STACK_SIZE region. If the PMM ever becomes non-sequential (e.g.
     * after buddy allocator introduction in Phase 5), this must be replaced
     * with a multi-page contiguous allocation.
     */
    uint8_t *stack = (void *)0;
    uint32_t i;
    for (i = 0; i < STACK_PAGES; i++) {
        uint64_t p = pmm_alloc_page();
        if (!p) {
            printk("[SCHED] FAIL: OOM allocating stack\n");
            for (;;) {}
        }
        if (i == 0)
            stack = (uint8_t *)(uintptr_t)p;
    }

    /* Set up the stack to look like ctx_switch already ran.
     *
     * ctx_switch pops: r15, r14, r13, r12, rbp, rbx, then ret → fn.
     * So the stack from low (RSP) to high must be:
     *   [r15=0][r14=0][r13=0][r12=0][rbp=0][rbx=0][fn]
     *
     * We build this by decrementing a pointer from stack_top:
     *   fn pushed first (deepest = highest address before RSP setup)
     *   then six zeros for the callee-saved regs
     *   RSP ends up pointing at the r15 slot.
     */
    uint64_t *sp = (uint64_t *)(stack + STACK_SIZE);
    *--sp = (uint64_t)(uintptr_t)fn;  /* return address: ret jumps here */
    *--sp = 0;                         /* rbx */
    *--sp = 0;                         /* rbp */
    *--sp = 0;                         /* r12 */
    *--sp = 0;                         /* r13 */
    *--sp = 0;                         /* r14 */
    *--sp = 0;                         /* r15  ← new task's RSP */

    task->rsp        = (uint64_t)(uintptr_t)sp;
    task->stack_base = stack;
    task->tid        = s_next_tid++;

    /* Add to circular list */
    if (!s_current) {
        task->next = task;
        s_current  = task;
    } else {
        /* Insert after current */
        task->next      = s_current->next;
        s_current->next = task;
    }

    s_task_count++;
}

void
sched_start(void)
{
    printk("[SCHED] OK: scheduler started, %u tasks\n", s_task_count);

    /* One-way switch into the first task.
     *
     * IMPORTANT: Do NOT call sti and return here. If we returned to the idle
     * hlt loop and the first PIT tick fired from there, sched_tick would call
     * ctx_switch(task_kbd, task_heartbeat) while RSP is deep in the ISR frame.
     * ctx_switch would save the ISR stack pointer into task_kbd->rsp, corrupting
     * the TCB. Resuming task_kbd later would load a garbage RSP and crash.
     *
     * Fix: switch directly into the first task using a stack-local dummy TCB.
     * ctx_switch saves our current RSP into dummy.rsp (which we immediately
     * abandon). The first task starts on its own correctly-constructed initial
     * stack. Each task enables interrupts at startup (arch-specific).
     *
     * sched_start() never returns.
     */
    if (!s_current) {
        printk("[SCHED] FAIL: sched_start called with no tasks\n");
        for (;;) {}
    }
    aegis_task_t dummy;
    ctx_switch(&dummy, s_current);
    __builtin_unreachable();
}

void
sched_tick(void)
{
    if (!s_current)                        /* no tasks spawned yet */
        return;
    if (s_current->next == s_current)      /* single task: nowhere to switch */
        return;

    aegis_task_t *old = s_current;
    s_current = s_current->next;

    /* ctx_switch is declared in arch.h with a forward struct declaration.
     * It saves old->rsp, loads s_current->rsp, and returns into new task. */
    ctx_switch(old, s_current);
}
