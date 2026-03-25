/* sys_signal.c — Signal handling syscalls */
#include "sys_impl.h"

/*
 * sys_rt_sigaction — syscall 13
 *
 * arg1 = signum, arg2 = user pointer to new k_sigaction_t (NULL = query),
 * arg3 = user pointer to old k_sigaction_t output (NULL = discard),
 * arg4 = sigset size in bytes (must be 8)
 *
 * Installs a signal handler for signum. Copies old handler to oldact if non-NULL.
 */
uint64_t
sys_rt_sigaction(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    if (arg4 != 8) return (uint64_t)-(int64_t)22; /* EINVAL */
    int signum = (int)arg1;
    if (signum <= 0 || signum >= 64) return (uint64_t)-(int64_t)22; /* EINVAL */
    /* SIGKILL, SIGSTOP, and SIGCONT cannot be caught or ignored */
    if (signum == SIGKILL || signum == SIGSTOP || signum == SIGCONT)
        return (uint64_t)-(int64_t)22; /* EINVAL */

    aegis_process_t *proc = (aegis_process_t *)sched_current();

    /* Copy out old action first (before overwriting) */
    if (arg3 != 0) {
        if (!user_ptr_valid(arg3, sizeof(k_sigaction_t)))
            return (uint64_t)-(int64_t)14; /* EFAULT */
        copy_to_user((void *)(uintptr_t)arg3, &proc->sigactions[signum],
                     sizeof(k_sigaction_t));
    }

    /* Install new action */
    if (arg2 != 0) {
        if (!user_ptr_valid(arg2, sizeof(k_sigaction_t)))
            return (uint64_t)-(int64_t)14; /* EFAULT */
        k_sigaction_t sa;
        copy_from_user(&sa, (const void *)(uintptr_t)arg2,
                       sizeof(k_sigaction_t));
        /* Validate handler: must be SIG_DFL (0), SIG_IGN (1),
         * or a user-space address — never a kernel address. */
        if (sa.sa_handler != SIG_DFL && sa.sa_handler != SIG_IGN) {
            if (!user_ptr_valid((uint64_t)(uintptr_t)sa.sa_handler, 1))
                return (uint64_t)-(int64_t)14; /* EFAULT */
        }
        /* Validate restorer similarly if provided */
        if (sa.sa_restorer != 0) {
            if (!user_ptr_valid((uint64_t)(uintptr_t)sa.sa_restorer, 1))
                return (uint64_t)-(int64_t)14; /* EFAULT */
        }
        proc->sigactions[signum] = sa;
    }
    return 0;
}

/*
 * sys_rt_sigprocmask — syscall 14
 *
 * arg1 = how (SIG_BLOCK=0, SIG_UNBLOCK=1, SIG_SETMASK=2)
 * arg2 = user pointer to new sigset_t (uint64_t; NULL = query only)
 * arg3 = user pointer to old sigset_t output (NULL = discard)
 * arg4 = sigset size in bytes (must be 8)
 */
uint64_t
sys_rt_sigprocmask(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    if (arg4 != 8) return (uint64_t)-(int64_t)22; /* EINVAL */

    aegis_process_t *proc = (aegis_process_t *)sched_current();

    /* Copy out old mask */
    if (arg3 != 0) {
        if (!user_ptr_valid(arg3, sizeof(uint64_t)))
            return (uint64_t)-(int64_t)14; /* EFAULT */
        copy_to_user((void *)(uintptr_t)arg3, &proc->signal_mask,
                     sizeof(uint64_t));
    }

    /* Apply new mask */
    if (arg2 != 0) {
        if (!user_ptr_valid(arg2, sizeof(uint64_t)))
            return (uint64_t)-(int64_t)14; /* EFAULT */
        uint64_t newset;
        copy_from_user(&newset, (const void *)(uintptr_t)arg2,
                       sizeof(uint64_t));
        switch (arg1) {
        case 0: proc->signal_mask |=  newset; break; /* SIG_BLOCK */
        case 1: proc->signal_mask &= ~newset; break; /* SIG_UNBLOCK */
        case 2: proc->signal_mask  =  newset; break; /* SIG_SETMASK */
        default: return (uint64_t)-(int64_t)22; /* EINVAL */
        }
        /* SIGKILL and SIGSTOP cannot be masked */
        proc->signal_mask &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));
    }
    return 0;
}

/*
 * sys_rt_sigreturn — syscall 15
 *
 * Called by musl's __restore_rt after a signal handler returns.
 * frame->user_rsp points at the rt_sigframe_t.pretcode slot.
 *
 * Reads rt_sigframe_t from user stack, patches frame->rip/rflags/user_rsp/r8/r9/r10
 * to restore the interrupted context, restores signal_mask from uc_sigmask,
 * and returns SIGRETURN_MAGIC to tell syscall_entry.asm to skip signal delivery.
 *
 * Phase 17 limitation: only rip/rflags/rsp/r8/r9/r10 are restored through the
 * frame mechanism. rbx/rbp/r12-r15/rax/rcx/rdx/rsi/rdi survive through the C
 * call chain per SysV ABI (callee-saved or not used by the handler).
 */
uint64_t
sys_rt_sigreturn(syscall_frame_t *frame)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();

    if (!user_ptr_valid(frame->user_rsp, sizeof(rt_sigframe_t))) {
        sched_exit(); /* signal frame corrupted — terminate */
        __builtin_unreachable();
    }
    rt_sigframe_t sf;
    copy_from_user(&sf, (const void *)(uintptr_t)frame->user_rsp,
                   sizeof(sf));

    /* Restore interrupted execution context into sysret frame slots */
    frame->rip      = (uint64_t)sf.gregs[REG_RIP];
    frame->rflags   = (uint64_t)sf.gregs[REG_EFL];
    frame->user_rsp = (uint64_t)sf.gregs[REG_RSP];
    frame->r8       = (uint64_t)sf.gregs[REG_R8];
    frame->r9       = (uint64_t)sf.gregs[REG_R9];
    frame->r10      = (uint64_t)sf.gregs[REG_R10];

    /* Restore signal mask from saved uc_sigmask */
    proc->signal_mask = sf.uc_sigmask;
    /* SIGKILL and SIGSTOP cannot be masked */
    proc->signal_mask &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));

    return SIGRETURN_MAGIC;
}

/*
 * sys_kill — syscall 62
 *
 * arg1 = pid (target process ID), arg2 = signum
 *
 * Sends signal signum to process with PID pid.
 * pid < 0: deliver to process group -pid.
 * pid == 0: deliver to calling process's own group.
 * pid > 0: deliver to individual process.
 * signal_send_pid internally guards is_user before treating a task as aegis_process_t.
 */
uint64_t
sys_kill(uint64_t arg1, uint64_t arg2)
{
    int32_t  pid = (int32_t)(uint32_t)arg1;
    int      sig = (int)arg2;
    if (sig <= 0 || sig >= 64) return (uint64_t)-(int64_t)22; /* EINVAL */
    if (pid < 0) {
        /* kill(-pgid, sig): deliver to entire process group */
        signal_send_pgrp((uint32_t)(-pid), sig);
        return 0;
    }
    if (pid == 0) {
        /* kill(0, sig): deliver to own process group */
        aegis_process_t *proc = (aegis_process_t *)sched_current();
        signal_send_pgrp(proc->pgid, sig);
        return 0;
    }
    signal_send_pid((uint32_t)pid, sig);
    return 0;
}

/*
 * sys_setfg — syscall 360 (Aegis private)
 *
 * arg1 = pid of the foreground process (0 = clear foreground)
 *
 * Registers the foreground PID with the keyboard driver so Ctrl-C sends
 * SIGINT to that process. The shell calls this before waitpid and clears
 * it after.
 */
uint64_t
sys_setfg(uint64_t arg1)
{
    kbd_set_tty_pgrp((uint32_t)arg1);
    return 0;
}
