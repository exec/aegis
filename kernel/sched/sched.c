#include "sched.h"
#include "arch.h"
#include "kva.h"
#include "pmm.h"
#include "printk.h"
#include "vmm.h"
#include "proc.h"
#include "fd_table.h"
#include "ext2.h"
#include <stddef.h>

/* Compile-time guard: ctx_switch.asm assumes sp is at offset 0 of TCB.
 * If anyone adds a field before sp, this catches it immediately. */
_Static_assert(offsetof(aegis_task_t, sp) == 0,
    "sp must be first field in aegis_task_t — ctx_switch depends on this");

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

    /* Allocate stack: STACK_PAGES usable pages plus one unmapped guard page
     * at the bottom.  Stack grows downward; the guard page causes a #PF on
     * overflow instead of silently corrupting adjacent KVA allocations.
     * The guard VA is permanently abandoned (bump allocator does not rewind). */
    uint8_t *stack_region = kva_alloc_pages(STACK_PAGES + 1);
    uint64_t guard_phys   = kva_page_phys(stack_region);
    vmm_unmap_page((uint64_t)(uintptr_t)stack_region);
    pmm_free_page(guard_phys);
    /* Usable stack starts one page above the (now-unmapped) guard page. */
    uint8_t *stack = stack_region + 4096UL;

    /* Set up the stack to look like ctx_switch already ran.
     * The frame layout must match the push/pop order in ctx_switch.asm/S. */
    uint64_t *sp = (uint64_t *)(stack + STACK_SIZE);
#ifdef __aarch64__
    /* ARM64 ctx_switch pushes 6 pairs via stp (x19/x20 ... x29/x30).
     * x30 (lr) = fn. Build from high to low matching ldp order:
     * [x19][x20] [x21][x22] [x23][x24] [x25][x26] [x27][x28] [x29][x30] */
    *--sp = 0;                          /* x20 */
    *--sp = 0;                          /* x19 */
    *--sp = 0;                          /* x22 */
    *--sp = 0;                          /* x21 */
    *--sp = 0;                          /* x24 */
    *--sp = 0;                          /* x23 */
    *--sp = 0;                          /* x26 */
    *--sp = 0;                          /* x25 */
    *--sp = 0;                          /* x28 */
    *--sp = 0;                          /* x27 */
    *--sp = (uint64_t)(uintptr_t)fn;   /* x30 (lr) — ret jumps here */
    *--sp = 0;                          /* x29 (fp) */
#else
    /* x86-64 ctx_switch pops: r15, r14, r13, r12, rbp, rbx, then ret → fn. */
    *--sp = (uint64_t)(uintptr_t)fn;   /* return address */
    *--sp = 0;                          /* rbx */
    *--sp = 0;                          /* rbp */
    *--sp = 0;                          /* r12 */
    *--sp = 0;                          /* r13 */
    *--sp = 0;                          /* r14 */
    *--sp = 0;                          /* r15 */
#endif

    task->sp               = (uint64_t)(uintptr_t)sp;
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
 * (ctx_switch writes dying->sp; the dying stack is live until the stack pointer switches).
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
        /* dying->exit_status was set by sys_exit before calling sched_exit. */

        /* Release the shared fd table (closes all fds if refcount drops to 0).
         *
         * Required for pipe correctness: write-end close fires sched_wake()
         * on any blocked reader, which must happen while the task is still
         * TASK_RUNNING (not TASK_ZOMBIE) so the woken task can be properly
         * scheduled.
         *
         * Ordering invariant: this runs before vmm_free_user_pml4
         * (wherever it is called). pipe_t lives in kva (kernel VA, always
         * accessible). Any future fd type whose close op touches user memory
         * must also rely on this ordering — do not move this call later. */
        fd_table_unref(dying->fd_table);
        dying->fd_table = (fd_table_t *)0;

        /* Mark self zombie — stays in run queue until waitpid reaps. */
        s_current->state = TASK_ZOMBIE;

        /* Notify parent of child exit via SIGCHLD.
         * signal_send_pid sets SIGCHLD pending on the parent and calls
         * sched_wake() if the parent is TASK_BLOCKED (sigsuspend path or
         * blocking waitpid path), transitioning it to TASK_RUNNING.
         * Must run before the woken_parent scan so the scan finds the
         * parent in TASK_RUNNING state. */
        if (dying->ppid != 0)
            signal_send_pid(dying->ppid, SIGCHLD);

        /* Find parent for direct ctx_switch (avoids PIT dependency).
         * signal_send_pid may have transitioned the parent BLOCKED→RUNNING;
         * check TASK_RUNNING here. */
        aegis_task_t *woken_parent = (void *)0;
        aegis_task_t *t = s_current->next;
        while (t != s_current) {
            if (t->is_user && t->state == TASK_RUNNING) {
                aegis_process_t *p = (aegis_process_t *)t;
                if (p->pid == dying->ppid &&
                    (t->waiting_for == 0 || t->waiting_for == dying->pid)) {
                    woken_parent = t;
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
            ext2_sync();    /* flush dirty blocks to NVMe before exit */
            printk("[AEGIS] System halted.\n");
            arch_request_shutdown();
        }

        /* Yield — zombie stays in queue until waitpid reaps.
         * Do NOT use the deferred-cleanup (g_prev_dying_tcb) path; that is
         * for non-zombie kernel task exits only.
         *
         * Direct-to-parent switch: if we woke a parent, switch to it
         * immediately instead of calling sched_yield_to_next().
         * sched_yield_to_next starts scanning from zombie->next in the
         * circular queue; when the queue is task_idle→parent→zombie,
         * zombie->next==task_idle and task_idle is picked first.  On AMD
         * bare metal the 8259A PIC IRQ0 may never reach the CPU (LAPIC not
         * in ExtINT/virtual-wire mode), so task_idle's sti+hlt never gets
         * preempted and the parent is never scheduled.  Switching directly
         * to the parent eliminates the PIT dependency for this path. */
        if (woken_parent) {
            aegis_task_t *zombie_task = s_current;
            s_current = woken_parent;
            arch_set_kernel_stack(s_current->kernel_stack_top);
            if (s_current->is_user)
                arch_set_fs_base(s_current->fs_base);
            ctx_switch(zombie_task, s_current);
            /* unreachable — zombie never resumes after direct switch */
        } else {
            sched_yield_to_next();
        }
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
        for (;;) arch_halt();
    }

    arch_set_kernel_stack(s_current->kernel_stack_top);

    /* If the next task is a user task, switch to its PML4. */
    if (s_current->is_user)
        vmm_switch_to(((aegis_process_t *)s_current)->pml4_phys);

    /* Record dying kernel task for deferred cleanup at the next sched_exit entry.
     * Must be set AFTER all list manipulation and BEFORE ctx_switch:
     * ctx_switch writes dying_k->sp, so the TCB must remain valid until
     * after the stack pointer switch completes. */
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
        arch_set_fs_base(s_current->fs_base);

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
    if (s_current->is_user)
        arch_set_fs_base(s_current->fs_base);
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
sched_stop(aegis_task_t *task)
{
    if (task != s_current) {
        /* Stopping a different task: just flip state. */
        task->state = TASK_STOPPED;
        return;
    }

    /* Self-stop: mirrors sched_block exactly, but sets TASK_STOPPED. */
    aegis_task_t *old = s_current;
    old->state = TASK_STOPPED;

    /* Advance s_current past the stopped task; skip non-RUNNING tasks.
     * task_idle guarantees the loop terminates. */
    s_current = old->next;
    while (s_current->state != TASK_RUNNING)
        s_current = s_current->next;

    arch_set_kernel_stack(s_current->kernel_stack_top);

    if (s_current->is_user)
        arch_set_fs_base(s_current->fs_base);

    ctx_switch(old, s_current);

    /* After ctx_switch returns (SIGCONT has resumed us), restore CR3 + FS.base.
     * Mirrors sched_block tail exactly. */
    if (s_current->is_user)
        vmm_switch_to(((aegis_process_t *)s_current)->pml4_phys);

    if (s_current->is_user)
        arch_set_fs_base(s_current->fs_base);
}

void
sched_resume(aegis_task_t *task)
{
    /* Mirrors sched_wake: flip state back to RUNNING.
     * Works for both TASK_STOPPED and TASK_BLOCKED (SIGCONT while blocked
     * on a read must also let the read return EINTR). */
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
        arch_set_fs_base(s_current->fs_base);

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
    if (s_current->is_user)
        arch_set_fs_base(s_current->fs_base);
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
     * ctx_switch would save the ISR stack pointer into task_kbd->sp, corrupting
     * the TCB. Resuming task_kbd later would load a garbage stack pointer and crash.
     *
     * Fix: switch directly into the first task using a stack-local dummy TCB.
     * ctx_switch saves our current stack pointer into dummy.sp (which we immediately
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
        arch_set_fs_base(s_current->fs_base);

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
     * It saves old->sp, loads s_current->sp, and returns into new task. */
    ctx_switch(old, s_current);

    /* Restore the incoming user process's FS base.
     * This must run AFTER ctx_switch returns (s_current is now the new task).
     * proc_enter_user handles only the first entry; preempted tasks resume
     * via isr_common_stub which does not reload FS.base. IF=0 here (PIT ISR). */
    if (s_current->is_user)
        arch_set_fs_base(s_current->fs_base);
}
