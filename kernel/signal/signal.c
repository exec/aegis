#include "signal.h"
#include "proc.h"
#include "sched.h"
#include "uaccess.h"
#include "printk.h"
#include "idt.h"        /* for cpu_state_t */
#include "syscall.h"    /* for syscall_frame_t */
#include <stdint.h>

void
signal_deliver(cpu_state_t *s)
{
    (void)s;
    /* Phase 17 Task 4 */
}

int
signal_deliver_sysret(syscall_frame_t *frame, uint64_t *saved_rdi_ptr)
{
    (void)frame; (void)saved_rdi_ptr;
    return 0;
}

void
signal_send_pid(uint32_t pid, int signum)
{
    if (pid == 0 || signum <= 0 || signum >= 64) return;

    /*
     * Walk the run queue to find the target process.
     * Single-core, IF=0 at call sites — no lock needed.
     */
    aegis_task_t *cur = sched_current();
    if (!cur) return;
    aegis_task_t *t   = cur;
    do {
        if (t->is_user) {
            aegis_process_t *p = (aegis_process_t *)t;
            if (p->pid == pid) {
                p->pending_signals |= (1ULL << (uint32_t)signum);
                /* Wake blocked process so it can check pending signals */
                if (t->state == TASK_BLOCKED)
                    sched_wake(t);
                return;
            }
        }
        t = t->next;
    } while (t != cur);
}

int
signal_check_pending(void)
{
    aegis_task_t *task = sched_current();
    if (!task) return 0;
    if (!task->is_user) return 0;
    aegis_process_t *proc = (aegis_process_t *)task;
    return (proc->pending_signals & ~proc->signal_mask) != 0;
}
