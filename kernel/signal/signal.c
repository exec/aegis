#include "signal.h"
#include "proc.h"
#include "sched.h"
#include "uaccess.h"
#include "syscall_util.h"   /* for user_ptr_valid */
#include "printk.h"
#include "idt.h"        /* for cpu_state_t */
#include "syscall.h"    /* for syscall_frame_t */
#include "vmm.h"        /* for vmm_switch_to, vmm_get_master_pml4 */
#include <stdint.h>

/*
 * signal_deliver — deliver the highest-priority pending signal when returning
 * to ring 3 via iretq. Called from isr.asm between isr_dispatch and
 * isr_post_dispatch, with CR3=master PML4 and IF=0.
 *
 * s->cs == 0x08 means returning to kernel mode (IRQ fired in kernel hlt loop) —
 * do not deliver. Only deliver to ring-3 (cs=0x23).
 */
void
signal_deliver(cpu_state_t *s)
{
    /* Only deliver to ring-3 returns */
    if (s->cs != 0x23) return;

    aegis_task_t *task = sched_current();
    if (!task || !task->is_user) return;
    aegis_process_t *proc = (aegis_process_t *)task;

    uint64_t deliverable = proc->pending_signals & ~proc->signal_mask;
    if (!deliverable) return;

    int signum = (int)__builtin_ctzll(deliverable);
    proc->pending_signals &= ~(1ULL << (uint32_t)signum);

    k_sigaction_t *sa = &proc->sigactions[signum];

    if (sa->sa_handler == SIG_DFL) {
        /* Default action */
        if (signum == SIGCHLD) return; /* default for SIGCHLD = ignore */
        /* All other defaults: terminate the process */
        sched_exit();  /* never returns */
    }

    if (sa->sa_handler == SIG_IGN) return;

    /* Validate handler and restorer are user-space addresses.
     * A kernel-VA handler would execute kernel code at ring-3 privilege.
     * user_ptr_valid checks address is canonical and below 0x800000000000. */
    if (!user_ptr_valid((uint64_t)sa->sa_handler, 1)) {
        sched_exit();  /* terminate process — bad handler address */
        return;
    }
    if (sa->sa_restorer && !user_ptr_valid((uint64_t)sa->sa_restorer, 1)) {
        sched_exit();  /* terminate process — bad restorer address */
        return;
    }

    /* User handler: build rt_sigframe_t on user stack, redirect iretq to handler */
    uint64_t user_rsp = s->rsp;  /* user RSP from the iretq frame */
    uint64_t new_rsp  = ((user_rsp - sizeof(rt_sigframe_t)) & ~15ULL) - 8;

    rt_sigframe_t sf;
    __builtin_memset(&sf, 0, sizeof(sf));
    sf.pretcode            = (uint64_t)sa->sa_restorer;
    /* Fill mcontext from cpu_state_t */
    sf.gregs[REG_R8]       = (int64_t)s->r8;
    sf.gregs[REG_R9]       = (int64_t)s->r9;
    sf.gregs[REG_R10]      = (int64_t)s->r10;
    sf.gregs[REG_R11]      = (int64_t)s->r11;
    sf.gregs[REG_R12]      = (int64_t)s->r12;
    sf.gregs[REG_R13]      = (int64_t)s->r13;
    sf.gregs[REG_R14]      = (int64_t)s->r14;
    sf.gregs[REG_R15]      = (int64_t)s->r15;
    sf.gregs[REG_RDI]      = (int64_t)s->rdi;
    sf.gregs[REG_RSI]      = (int64_t)s->rsi;
    sf.gregs[REG_RBP]      = (int64_t)s->rbp;
    sf.gregs[REG_RBX]      = (int64_t)s->rbx;
    sf.gregs[REG_RDX]      = (int64_t)s->rdx;
    sf.gregs[REG_RAX]      = (int64_t)s->rax;
    sf.gregs[REG_RCX]      = (int64_t)s->rcx;
    sf.gregs[REG_RSP]      = (int64_t)s->rsp;
    sf.gregs[REG_RIP]      = (int64_t)s->rip;
    sf.gregs[REG_EFL]      = (int64_t)s->rflags;
    sf.gregs[REG_CSGSFS]   = (int64_t)s->cs;
    sf.uc_sigmask          = proc->signal_mask;

    /* Validate the destination before writing — terminate if the frame
     * address is not in user space (signal stack overflow or bad RSP). */
    if (!user_ptr_valid(new_rsp, sizeof(sf))) {
        sched_exit();  /* never returns */
    }

    /* signal_deliver is called while CR3 = master PML4 (isr_common_stub
     * switches CR3 before calling isr_dispatch and this function).
     * The user stack pages are only mapped in the process's user PML4.
     * Switch to the user PML4 for copy_to_user, then back to master so
     * the rest of the kernel (isr_post_dispatch, printk, etc.) can access
     * kva-mapped objects.  isr_post_dispatch will restore the saved CR3
     * from the stack slot after this function returns. */
    vmm_switch_to(proc->pml4_phys);
    copy_to_user((void *)new_rsp, &sf, sizeof(sf));
    vmm_switch_to(vmm_get_master_pml4());

    /* Redirect iretq to handler */
    s->rip    = (uint64_t)sa->sa_handler;
    s->rsp    = new_rsp;
    s->rdi    = (uint64_t)signum;  /* first arg to handler */
    s->rax    = 0;

    /* Mask: block this signal and sa_mask while handler runs */
    proc->signal_mask |= sa->sa_mask | (1ULL << (uint32_t)signum);
}

/*
 * signal_deliver_sysret — deliver pending signal on the syscall return path.
 *
 * frame:          syscall_frame_t * — patch rip/user_rsp for user handler delivery
 * saved_rdi_ptr:  pointer to saved user rdi slot on kernel stack; write signum here
 *                 so that `pop rdi` in syscall_entry.asm loads the signal number
 *                 as the first argument to the handler.
 *
 * Returns 0 if no signal or SIG_DFL action (sched_exit never returns for SIG_DFL).
 * Returns 1 if a user handler was installed (caller sets rax=0, does sysret).
 *
 * Phase 17 limitation: only rip/rflags/user_rsp/r8/r9/r10 are saved in
 * syscall_frame_t. rbx/rbp/r11-r15/rax/rcx/rdx/rsi/rdi from the interrupted
 * context are not restored into the signal frame gregs. Signal handlers that
 * rely on exact register restoration of these fields will not work correctly.
 * SIG_DFL (sched_exit) and simple SIGCHLD handlers work fine.
 */
int
signal_deliver_sysret(syscall_frame_t *frame, uint64_t *saved_rdi_ptr)
{
    aegis_task_t *task = sched_current();
    if (!task || !task->is_user) return 0;
    aegis_process_t *proc = (aegis_process_t *)task;

    uint64_t deliverable = proc->pending_signals & ~proc->signal_mask;
    if (!deliverable) return 0;

    int signum = (int)__builtin_ctzll(deliverable);
    proc->pending_signals &= ~(1ULL << (uint32_t)signum);

    k_sigaction_t *sa = &proc->sigactions[signum];

    if (sa->sa_handler == SIG_DFL) {
        if (signum == SIGCHLD) return 0; /* ignore */
        sched_exit();  /* never returns */
    }

    if (sa->sa_handler == SIG_IGN) return 0;

    /* Validate handler and restorer are user-space addresses.
     * A kernel-VA handler would execute kernel code at ring-3 privilege.
     * user_ptr_valid checks address is canonical and below 0x800000000000. */
    if (!user_ptr_valid((uint64_t)sa->sa_handler, 1)) {
        sched_exit();  /* terminate process — bad handler address */
        return 0;
    }
    if (sa->sa_restorer && !user_ptr_valid((uint64_t)sa->sa_restorer, 1)) {
        sched_exit();  /* terminate process — bad restorer address */
        return 0;
    }

    /* User handler: build rt_sigframe_t on user stack */
    uint64_t user_rsp = frame->user_rsp;
    uint64_t new_rsp  = ((user_rsp - sizeof(rt_sigframe_t)) & ~15ULL) - 8;

    rt_sigframe_t sf;
    __builtin_memset(&sf, 0, sizeof(sf));
    sf.pretcode          = (uint64_t)sa->sa_restorer;
    sf.gregs[REG_R8]     = (int64_t)frame->r8;
    sf.gregs[REG_R9]     = (int64_t)frame->r9;
    sf.gregs[REG_R10]    = (int64_t)frame->r10;
    sf.gregs[REG_RIP]    = (int64_t)frame->rip;
    sf.gregs[REG_EFL]    = (int64_t)frame->rflags;
    sf.gregs[REG_RSP]    = (int64_t)frame->user_rsp;
    sf.uc_sigmask        = proc->signal_mask;

    if (!user_ptr_valid(new_rsp, sizeof(sf))) {
        sched_exit();  /* never returns */
    }

    copy_to_user((void *)new_rsp, &sf, sizeof(sf));

    /* Patch sysret frame: return to handler instead of original RIP */
    frame->rip      = (uint64_t)sa->sa_handler;
    frame->user_rsp = new_rsp;

    /* Write signum to the saved rdi slot so pop rdi loads it as handler arg1 */
    *saved_rdi_ptr = (uint64_t)signum;

    proc->signal_mask |= sa->sa_mask | (1ULL << (uint32_t)signum);
    return 1;
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
