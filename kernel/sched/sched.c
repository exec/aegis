#include "sched.h"
#include "arch.h"
#include "kva.h"
#include "printk.h"
#include "vmm.h"
#include "proc.h"
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
    /* Allocate TCB (one kva page — higher-half VA, no identity-map dependency). */
    aegis_task_t *task = kva_alloc_pages(1);

    /* Allocate stack (STACK_PAGES kva pages — consecutive VAs, no contiguity
     * assumption on physical addresses). */
    uint8_t *stack = kva_alloc_pages(STACK_PAGES);

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

    task->rsp              = (uint64_t)(uintptr_t)sp;
    task->stack_base       = stack;
    task->kernel_stack_top = (uint64_t)(uintptr_t)(stack + STACK_SIZE);
    task->is_user          = 0;
    task->tid              = s_next_tid++;
    task->stack_pages      = STACK_PAGES;
    task->state            = TASK_RUNNING;
    task->waiting_for      = 0;

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
sched_add(aegis_task_t *task)
{
    if (!s_current) {
        task->next = task;
        s_current  = task;
    } else {
        task->next      = s_current->next;
        s_current->next = task;
    }
    s_task_count++;
}

/* Deferred cleanup: dying task's resources cannot be freed before ctx_switch
 * (ctx_switch writes dying->rsp; the dying stack is live until RSP switches).
 * Record them here and free at the entry of the next sched_exit call. */
static void    *g_prev_dying_tcb         = NULL;
static void    *g_prev_dying_stack       = NULL;
static uint64_t g_prev_dying_stack_pages = 0;

void
sched_exit(void)
{
    /* ── Deferred cleanup from the PREVIOUS exiting kernel/orphaned task ──
     * Free TCB + kernel stack of the task that exited last time.
     * Safe: ctx_switch has completed; that TCB and stack are no longer live
     * on any CPU. Must be at the TOP of sched_exit, before any new exit logic. */
    if (g_prev_dying_tcb) {
        kva_free_pages(g_prev_dying_stack, g_prev_dying_stack_pages);
        kva_free_pages(g_prev_dying_tcb, 1);
        g_prev_dying_tcb = NULL;
    }

    /* Switch to master PML4 so kernel structures are safely accessible.
     *
     * Before Phase 7: required because TCBs were in identity-mapped [0..4MB),
     * which is absent from user PML4s.
     * After Phase 7: TCBs are kva-mapped higher-half VAs, visible from any CR3
     * (pd_hi is shared). The switch is retained as a defensive measure. */
    vmm_switch_to(vmm_get_master_pml4());

    if (s_current->is_user) {
        aegis_process_t *dying = (aegis_process_t *)s_current;
        int fd_i;
        /* dying->exit_status was set by sys_exit before calling sched_exit. */

        /* Close all open fds before entering zombie state.
         *
         * Required for pipe correctness: write-end close fires sched_wake()
         * on any blocked reader, which must happen while the task is still
         * TASK_RUNNING (not TASK_ZOMBIE) so the woken task can be properly
         * scheduled.
         *
         * Ordering invariant: this loop runs before vmm_free_user_pml4
         * (wherever it is called). pipe_t lives in kva (kernel VA, always
         * accessible). Any future fd type whose close op touches user memory
         * must also rely on this ordering — do not move this loop later. */
        for (fd_i = 0; fd_i < PROC_MAX_FDS; fd_i++) {
            if (dying->fds[fd_i].ops) {
                dying->fds[fd_i].ops->close(dying->fds[fd_i].priv);
                dying->fds[fd_i].ops = NULL;
            }
        }

        /* Mark self zombie — stays in run queue until waitpid reaps. */
        s_current->state = TASK_ZOMBIE;

        /* Wake blocked parent waiting for this child, if any. */
        aegis_task_t *t = s_current->next;
        while (t != s_current) {
            if (t->is_user && t->state == TASK_BLOCKED) {
                aegis_process_t *p = (aegis_process_t *)t;
                if (p->pid == dying->ppid &&
                    (t->waiting_for == 0 || t->waiting_for == dying->pid)) {
                    sched_wake(t);
                    break;
                }
            }
            t = t->next;
        }

        /* Shutdown detection: scan for remaining RUNNING user tasks.
         * Zombies have is_user=1 but state==TASK_ZOMBIE — they are not live.
         * Without the state check, this scan would never trigger shutdown
         * because the just-zombified task is still in the queue with is_user=1. */
        int live_users = 0;
        t = s_current->next;
        while (t != s_current) {
            if (t->is_user && t->state != TASK_ZOMBIE)
                live_users = 1;
            t = t->next;
        }
        if (!live_users) {
            printk("[AEGIS] System halted.\n");
            arch_request_shutdown();
        }

        /* Yield — zombie stays in queue until waitpid reaps.
         * Do NOT use the deferred-cleanup (g_prev_dying_tcb) path; that is
         * for non-zombie kernel task exits only. */
        sched_yield_to_next();
        /* unreachable — zombie never resumes */
        for (;;) {}
    }

    /* ── Kernel task (non-user) exit path ── */

    /* IF=0 throughout (IA32_SFMASK cleared IF on SYSCALL entry) —
     * no preemption can occur during list manipulation. */
    aegis_task_t *prev = s_current;
    while (prev->next != s_current)
        prev = prev->next;

    aegis_task_t *dying_k = s_current;
    s_current             = dying_k->next;
    prev->next            = s_current;
    s_task_count--;

    if (s_current == dying_k) {  /* last task — everything has exited */
        arch_request_shutdown();
        for (;;) __asm__ volatile ("hlt");
    }

    arch_set_kernel_stack(s_current->kernel_stack_top);

    /* If the next task is a user task, switch to its PML4. */
    if (s_current->is_user)
        vmm_switch_to(((aegis_process_t *)s_current)->pml4_phys);

    /* Record dying kernel task for deferred cleanup at the next sched_exit entry.
     * Must be set AFTER all list manipulation and BEFORE ctx_switch:
     * ctx_switch writes dying_k->rsp, so the TCB must remain valid until
     * after the RSP switch completes. */
    g_prev_dying_stack       = (void *)dying_k->stack_base;
    g_prev_dying_stack_pages = dying_k->stack_pages;
    g_prev_dying_tcb         = dying_k;
    ctx_switch(dying_k, s_current);
    __builtin_unreachable();
}

void
sched_block(void)
{
    aegis_task_t *old = s_current;

    old->state = TASK_BLOCKED;

    /* Leave old in the circular run queue with state=TASK_BLOCKED.
     *
     * Rationale: sched_exit's zombie-wakeup scan traverses the run queue
     * looking for blocked parents (state==TASK_BLOCKED).  If sched_block
     * removed the task from the queue, the scan could never find it and the
     * parent would never be woken — the shell would stay blocked forever
     * after the child exited.
     *
     * sched_tick and sched_yield_to_next already skip non-RUNNING tasks,
     * so leaving BLOCKED tasks in the queue is safe.  sched_wake() simply
     * transitions state back to TASK_RUNNING; no re-insertion is needed.
     */

    /* Advance s_current past the blocked task; skip non-RUNNING tasks.
     * task_idle guarantees the loop terminates. */
    s_current = old->next;
    while (s_current->state != TASK_RUNNING)
        s_current = s_current->next;

    /* Update TSS RSP0 and g_kernel_rsp for the incoming task before
     * ctx_switch so the next syscall from this task uses its own kernel
     * stack, not the stack of whatever task ran last. */
    arch_set_kernel_stack(s_current->kernel_stack_top);

    /* Set FS.base for the incoming task before ctx_switch. */
    if (s_current->is_user)
        arch_set_fs_base(((aegis_process_t *)s_current)->fs_base);

    ctx_switch(old, s_current);

    /* Restore CR3 for the incoming user task after ctx_switch returns.
     * sched_exit switches to master PML4 before context-switching to this
     * task (via sched_yield_to_next).  Without this restore, a user task
     * that was unblocked by sched_exit would resume with master PML4 loaded,
     * causing any copy_to_user call (e.g. sys_waitpid wstatus write) to #PF
     * because user stack pages are only mapped in the process's user PML4. */
    if (s_current->is_user)
        vmm_switch_to(((aegis_process_t *)s_current)->pml4_phys);

    /* Restore FS.base for the incoming user task after ctx_switch returns.
     * sched_tick does this in its path; sched_block must mirror it.
     * Without this, a user task that was blocked while another user task
     * ran and set a different FS_BASE would resume with the wrong FS_BASE,
     * corrupting TLS access (__errno_location, stack canary, etc.). */
    if (s_current->is_user) {
        aegis_process_t *p = (aegis_process_t *)s_current;
        arch_set_fs_base(p->fs_base);
    }
}

void
sched_wake(aegis_task_t *task)
{
    /* The task remains in the circular run queue (sched_block no longer
     * removes it).  Simply transition state back to TASK_RUNNING so the
     * scheduler's next pass will pick it up.  No list insertion needed. */
    task->state = TASK_RUNNING;
}

void
sched_yield_to_next(void)
{
    aegis_task_t *old = s_current;
    do {
        s_current = s_current->next;
    } while (s_current->state != TASK_RUNNING);
    arch_set_kernel_stack(s_current->kernel_stack_top);

    /* Set FS.base for the incoming task before ctx_switch. */
    if (s_current->is_user)
        arch_set_fs_base(((aegis_process_t *)s_current)->fs_base);

    ctx_switch(old, s_current);

    /* Restore CR3 for the incoming user task after ctx_switch returns.
     * sched_exit calls vmm_switch_to(master_pml4) at its top, then calls
     * sched_yield_to_next to switch away from the dying task.  The task
     * that resumes here would have master PML4 loaded; any subsequent
     * copy_to_user (e.g. sys_waitpid wstatus write) would #PF because the
     * user stack pages are only mapped in the process's user PML4. */
    if (s_current->is_user)
        vmm_switch_to(((aegis_process_t *)s_current)->pml4_phys);

    /* Restore FS.base for the incoming user task after ctx_switch returns.
     * sched_tick does this in its path; sched_yield_to_next must mirror it.
     * Without this, a user task that was blocked while another user task
     * ran and set a different FS_BASE would resume with the wrong FS_BASE,
     * corrupting TLS access (__errno_location, stack canary, etc.). */
    if (s_current->is_user) {
        aegis_process_t *p = (aegis_process_t *)s_current;
        arch_set_fs_base(p->fs_base);
    }
}

void
sched_start(void)
{
    if (!s_current) {
        printk("[SCHED] FAIL: sched_start called with no tasks\n");
        for (;;) {}
    }

    printk("[SCHED] OK: scheduler started, %u tasks\n", s_task_count);

    /* One-way switch into the first task.
     *
     * IMPORTANT: Do NOT enable interrupts and return here. If we returned to
     * the idle loop and the first timer tick fired from there, sched_tick would call
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
    arch_set_kernel_stack(s_current->kernel_stack_top);
    /* sched_start always enters the first task (kbd, a kernel task).
     * No CR3 switch here: if the first task is a user task, proc_enter_user
     * handles the PML4 switch before iretq (same as the timer-preemption
     * first-entry path). */

    aegis_task_t dummy;
    ctx_switch(&dummy, s_current);
    __builtin_unreachable();
}

aegis_task_t *
sched_current(void)
{
    return s_current;
}

void
sched_tick(void)
{
    if (!s_current)                        /* no tasks spawned yet */
        return;
    if (s_current->next == s_current)      /* single task: nowhere to switch */
        return;

    aegis_task_t *old = s_current;
    /* Skip blocked/zombie tasks. task_idle guarantees termination. */
    do {
        s_current = s_current->next;
    } while (s_current->state != TASK_RUNNING);

    arch_set_kernel_stack(s_current->kernel_stack_top);

    /* Set FS.base for the incoming task before ctx_switch so that the task
     * enters user space (or resumes) with the correct TLS pointer.
     * Must be paired with the arch_set_fs_base after ctx_switch (for the
     * outgoing task's subsequent resume). */
    if (s_current->is_user)
        arch_set_fs_base(((aegis_process_t *)s_current)->fs_base);

    /*
     * CR3 switch policy in sched_tick (Phase 5):
     *
     * sched_tick always runs inside isr_common_stub which switches to the
     * master PML4 at interrupt entry.  sched_tick therefore always executes
     * with the master PML4 loaded, regardless of whether the interrupted task
     * was a kernel or user task.
     *
     * (a) Switching TO a user task: do NOT switch CR3 here.  The switch to
     *     the user PML4 is performed by proc_enter_user (first entry) or by
     *     isr_common_stub's saved-CR3 restore (subsequent preemptions).
     *
     *     CRITICAL: sched_tick runs on the OUTGOING kernel task's kva-mapped
     *     stack.  Calling vmm_switch_to(user_pml4) from mid-sched_tick would
     *     switch away from the task being context-switched out while its stack
     *     is still live on the CPU — the next stack access would use the wrong
     *     CR3 context.  CR3 switches happen only in proc_enter_user (ring-3
     *     entry) and sched_exit (task teardown).
     *
     * (b) Switching FROM a user task to a kernel task: isr_common_stub
     *     already switched to master PML4 at interrupt entry.  No further
     *     CR3 switch is needed here.
     */

    /* ctx_switch is declared in arch.h with a forward struct declaration.
     * It saves old->rsp, loads s_current->rsp, and returns into new task. */
    ctx_switch(old, s_current);

    /* Restore the incoming user process's FS base.
     * This must run AFTER ctx_switch returns (s_current is now the new task).
     * proc_enter_user handles only the first entry; preempted tasks resume
     * via isr_common_stub which does not reload FS.base. IF=0 here (PIT ISR). */
    if (s_current->is_user) {
        aegis_process_t *p = (aegis_process_t *)s_current;
        arch_set_fs_base(p->fs_base);
    }
}
