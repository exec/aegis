#include "signal.h"
#include "proc.h"
#include "sched.h"
#include "printk.h"
#include <stdint.h>

#ifdef __aarch64__
/* ARM64 signal delivery — builds a signal frame on the user stack
 * and redirects ELR to the signal handler. On sigreturn, the saved
 * registers are restored from the frame. */
#include "idt.h"
#include "syscall.h"
#include "uaccess.h"
#include "syscall_util.h"
#include "vmm.h"

void
signal_deliver(cpu_state_t *s)
{
    aegis_task_t *task = sched_current();
    if (!task || !task->is_user) return;

    aegis_process_t *proc = (aegis_process_t *)task;
    uint64_t deliverable = proc->pending_signals & ~proc->signal_mask;
    if (!deliverable) return;

    /* Find lowest pending signal */
    int signum;
    for (signum = 1; signum < 32; signum++) {
        if (deliverable & (1ULL << signum)) break;
    }
    if (signum >= 32) return;

    k_sigaction_t *sa = &proc->sigactions[signum];

    /* SIG_IGN — clear and return */
    if (sa->sa_handler == SIG_IGN) {
        proc->pending_signals &= ~(1ULL << signum);
        return;
    }

    /* SIG_DFL — terminate for most signals */
    if (sa->sa_handler == SIG_DFL) {
        proc->pending_signals &= ~(1ULL << signum);
        if (signum == SIGCHLD || signum == SIGCONT) return; /* ignore */
        proc->exit_status = 128 + signum;
        sched_exit();
        __builtin_unreachable();
    }

    /* Build signal frame on user stack */
    uint64_t user_sp = s->sp_el0;
    uint64_t new_sp = (user_sp - sizeof(rt_sigframe_t)) & ~0xFUL; /* 16-byte align */

    /* Validate new_sp */
    if (new_sp >= user_sp || new_sp > USER_ADDR_MAX || new_sp < 0x1000) {
        proc->exit_status = 128 + signum;
        sched_exit();
        __builtin_unreachable();
    }

    rt_sigframe_t sf;
    /* Zero the frame */
    {
        uint8_t *p = (uint8_t *)&sf;
        uint64_t i;
        for (i = 0; i < sizeof(sf); i++) p[i] = 0;
    }

    /* Save registers into gregs */
    {
        int i;
        for (i = 0; i < 31; i++)
            sf.gregs[i] = (int64_t)s->x[i];
        sf.gregs[REG_SP]     = (int64_t)s->sp_el0;
        sf.gregs[REG_PC]     = (int64_t)s->elr;
        sf.gregs[REG_PSTATE] = (int64_t)s->spsr;
    }

    sf.pretcode  = (uint64_t)sa->sa_restorer;
    sf.uc_sigmask = proc->signal_mask;

    /* Copy frame to user stack */
    copy_to_user((void *)(uintptr_t)new_sp, &sf, sizeof(sf));

    /* Redirect execution to signal handler */
    s->elr    = (uint64_t)sa->sa_handler;
    s->sp_el0 = new_sp;
    s->x[0]   = (uint64_t)signum;  /* first argument = signal number */

    /* Block the signal during handler execution */
    proc->signal_mask |= sa->sa_mask | (1ULL << signum);
    proc->signal_mask &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));

    /* Clear pending */
    proc->pending_signals &= ~(1ULL << signum);
}

int
signal_deliver_sysret(syscall_frame_t *frame, uint64_t *saved_rdi_ptr)
{
    /* On ARM64, we only use the iretq-equivalent (ERET) path.
     * The sysret path is x86-specific. Check for pending signals
     * and deliver via the frame if needed. */
    (void)saved_rdi_ptr;

    aegis_task_t *task = sched_current();
    if (!task || !task->is_user) return 0;

    aegis_process_t *proc = (aegis_process_t *)task;
    uint64_t deliverable = proc->pending_signals & ~proc->signal_mask;
    if (!deliverable) return 0;

    /* Find lowest pending signal */
    int signum;
    for (signum = 1; signum < 32; signum++) {
        if (deliverable & (1ULL << signum)) break;
    }
    if (signum >= 32) return 0;

    k_sigaction_t *sa = &proc->sigactions[signum];

    if (sa->sa_handler == SIG_IGN) {
        proc->pending_signals &= ~(1ULL << signum);
        return 0;
    }
    if (sa->sa_handler == SIG_DFL) {
        proc->pending_signals &= ~(1ULL << signum);
        if (signum == SIGCHLD || signum == SIGCONT) return 0;
        proc->exit_status = 128 + signum;
        sched_exit();
        __builtin_unreachable();
    }

    /* Build frame on user stack and redirect */
    uint64_t user_sp = frame->user_sp;
    uint64_t new_sp = (user_sp - sizeof(rt_sigframe_t)) & ~0xFUL;

    if (new_sp >= user_sp || new_sp > USER_ADDR_MAX || new_sp < 0x1000) {
        proc->exit_status = 128 + signum;
        sched_exit();
        __builtin_unreachable();
    }

    rt_sigframe_t sf;
    {
        uint8_t *p = (uint8_t *)&sf;
        uint64_t i;
        for (i = 0; i < sizeof(sf); i++) p[i] = 0;
    }

    /* Save registers from syscall frame */
    {
        int i;
        for (i = 0; i < 31; i++)
            sf.gregs[i] = (int64_t)frame->regs[i];
        sf.gregs[REG_SP]     = (int64_t)frame->user_sp;
        sf.gregs[REG_PC]     = (int64_t)frame->elr;
        sf.gregs[REG_PSTATE] = (int64_t)frame->spsr;
    }

    sf.pretcode   = (uint64_t)sa->sa_restorer;
    sf.uc_sigmask = proc->signal_mask;

    copy_to_user((void *)(uintptr_t)new_sp, &sf, sizeof(sf));

    FRAME_IP(frame) = (uint64_t)sa->sa_handler;
    FRAME_SP(frame) = new_sp;
    frame->regs[0]  = (uint64_t)signum;

    proc->signal_mask |= sa->sa_mask | (1ULL << signum);
    proc->signal_mask &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));
    proc->pending_signals &= ~(1ULL << signum);

    return 1;
}

#else /* x86-64 */
#include "arch.h"
#include "uaccess.h"
#include "syscall_util.h"
#include "idt.h"
#include "syscall.h"
#include "vmm.h"

/*
 * signal_deliver — deliver the highest-priority pending signal when returning
 * to ring 3 via iretq. Called from isr.asm between isr_dispatch and
 * isr_post_dispatch, with CR3=master PML4 and IF=0.
 *
 * s->cs == ARCH_KERNEL_CS means returning to kernel mode (IRQ fired in kernel
 * hlt loop) — do not deliver. Only deliver to ring-3 (cs=ARCH_USER_CS).
 */
void
signal_deliver(cpu_state_t *s)
{
    /* Only deliver to ring-3 returns */
    if (s->cs != ARCH_USER_CS) return;

    aegis_task_t *task = sched_current();
    if (!task || !task->is_user) return;
    aegis_process_t *proc = (aegis_process_t *)task;

    uint64_t deliverable = proc->pending_signals & ~proc->signal_mask;
    if (!deliverable) return;

    int signum = (int)__builtin_ctzll(deliverable);
    proc->pending_signals &= ~(1ULL << (uint32_t)signum);

    k_sigaction_t *sa = &proc->sigactions[signum];

    if (sa->sa_handler == SIG_DFL) {
        if (signum == SIGCHLD || signum == SIGCONT) return; /* ignore */
        if (signum == SIGTSTP || signum == SIGSTOP ||
            signum == SIGTTIN || signum == SIGTTOU) {
            proc->stop_signum = (uint32_t)signum;
            /* Notify parent in case it's blocked in waitpid(WUNTRACED) */
            signal_send_pid(proc->ppid, SIGCHLD);
            sched_stop((aegis_task_t *)task);  /* yields; returns on SIGCONT */
            return;
        }
        sched_exit();  /* terminate — all other SIG_DFL signals */
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
 * Limitation: only rip/rflags/user_rsp/r8/r9/r10 are saved in
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
        if (signum == SIGCHLD || signum == SIGCONT) return 0; /* ignore */
        if (signum == SIGTSTP || signum == SIGSTOP ||
            signum == SIGTTIN || signum == SIGTTOU) {
            proc->stop_signum = (uint32_t)signum;
            signal_send_pid(proc->ppid, SIGCHLD);
            sched_stop((aegis_task_t *)task);  /* yields; returns on SIGCONT */
            return 0;
        }
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

#endif /* !__aarch64__ — end of arch-specific signal_deliver/signal_deliver_sysret */

/* ── Architecture-agnostic signal functions ──────────────────────── */

void
signal_send_pid(uint32_t pid, int signum)
{
    if (pid == 0 || signum <= 0 || signum >= 64) return;

    aegis_task_t *cur = sched_current();
    if (!cur) return;
    aegis_task_t *t = cur;
    do {
        if (t->is_user) {
            aegis_process_t *p = (aegis_process_t *)t;
            if (p->pid == pid) {
                p->pending_signals |= (1ULL << (uint32_t)signum);
                if (t->state == TASK_BLOCKED)
                    sched_wake(t);
                return;
            }
        }
        t = t->next;
    } while (t != cur);
}

void
signal_send_pgrp(uint32_t pgid, int signum)
{
    if (pgid == 0 || signum <= 0 || signum >= 64) return;

    aegis_task_t *cur = sched_current();
    if (!cur) return;
    aegis_task_t *t = cur;
    do {
        if (t->is_user) {
            aegis_process_t *p = (aegis_process_t *)t;
            if (p->pgid == pgid && p->pid != 1) {
                p->pending_signals |= (1ULL << (uint32_t)signum);
                if (t->state == TASK_STOPPED)
                    sched_resume(t);
                else if (t->state == TASK_BLOCKED)
                    sched_wake(t);
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
