/* sys_process.c — Process lifecycle syscalls: exit, fork, clone, waitpid */
#include "sys_impl.h"
#include "futex.h"
#include "vma.h"
#include "tty.h"

static uint32_t s_fork_count = 1;  /* starts at 1 for init */

uint32_t proc_fork_count(void) { return s_fork_count; }
void     proc_inc_fork_count(void) { s_fork_count++; }

/* ── Clone flags (Linux ABI) ──────────────────────────────────────────────── */
#define CLONE_VM             0x00000100u
#define CLONE_FS             0x00000200u
#define CLONE_FILES          0x00000400u
#define CLONE_SIGHAND        0x00000800u
#define CLONE_VFORK          0x00004000u
#define CLONE_THREAD         0x00010000u
#define CLONE_SYSVSEM        0x00040000u
#define CLONE_SETTLS         0x00080000u
#define CLONE_PARENT_SETTID  0x00100000u
#define CLONE_CHILD_CLEARTID 0x00200000u
#define CLONE_CHILD_SETTID   0x01000000u

/*
 * sys_exit — syscall 60
 *
 * arg1 = exit code (stored in proc->exit_status, returned by waitpid)
 * Calls sched_exit() which never returns.
 */
uint64_t
sys_exit(uint64_t arg1)
{
    if (sched_current()->is_user) {
        aegis_process_t *proc = (aegis_process_t *)sched_current();
        proc->exit_status = arg1 & 0xFF;

        /* clear_child_tid: write 0 and futex wake (for pthread_join) */
        if (sched_current()->clear_child_tid) {
            uint32_t zero = 0;
            vmm_write_user_bytes(proc->pml4_phys,
                                 sched_current()->clear_child_tid,
                                 &zero, sizeof(zero));
            futex_wake_addr(sched_current()->clear_child_tid, 1);
        }

        /* Session leader exit: SIGHUP + SIGCONT to foreground group */
        if (proc->pid == proc->sid) {
            tty_t *ctty = tty_find_controlling(proc->sid);
            if (ctty && ctty->fg_pgrp) {
                signal_send_pgrp(ctty->fg_pgrp, SIGHUP);
                signal_send_pgrp(ctty->fg_pgrp, SIGCONT);
                ctty->session_id = 0; /* disassociate terminal */
            }
        }

        if (proc->pid == 1) {
            printk("[INIT] PID 1 exited with status %u — halting\n",
                   (uint32_t)(arg1 & 0xFF));
            arch_request_shutdown();
        }

        /* C2: Reparent orphan children to init (PID 1) so they can be
         * reaped by waitpid instead of staying as zombies forever. */
        {
            aegis_task_t *t = sched_current()->next;
            while (t != sched_current()) {
                if (t->is_user) {
                    aegis_process_t *child = (aegis_process_t *)t;
                    if (child->ppid == proc->pid)
                        child->ppid = 1;
                }
                t = t->next;
            }
        }
    }
    sched_exit();
    __builtin_unreachable();
}

/* ── exit_group ────────────────────────────────────────────────────────── */
uint64_t sys_exit_group(uint64_t arg1)
{
    aegis_task_t *cur = sched_current();
    if (cur->is_user) {
        aegis_process_t *proc = (aegis_process_t *)cur;
        proc->exit_status = arg1 & 0xFF;

        /* Kill all other threads in the same thread group */
        uint32_t my_tgid = proc->tgid;
        aegis_task_t *t = cur->next;
        while (t != cur) {
            if (t->is_user) {
                aegis_process_t *tp = (aegis_process_t *)t;
                if (tp->tgid == my_tgid) {
                    t->state = TASK_ZOMBIE;
                    /* Do clear_child_tid for killed threads too */
                    if (t->clear_child_tid) {
                        uint32_t zero = 0;
                        vmm_write_user_bytes(tp->pml4_phys,
                                             t->clear_child_tid,
                                             &zero, sizeof(zero));
                        futex_wake_addr(t->clear_child_tid, 1);
                    }
                }
            }
            t = t->next;
        }

        /* Session leader exit: SIGHUP + SIGCONT to foreground group */
        if (proc->pid == proc->sid) {
            tty_t *ctty = tty_find_controlling(proc->sid);
            if (ctty && ctty->fg_pgrp) {
                signal_send_pgrp(ctty->fg_pgrp, SIGHUP);
                signal_send_pgrp(ctty->fg_pgrp, SIGCONT);
                ctty->session_id = 0; /* disassociate terminal */
            }
        }

        if (proc->pid == 1 || proc->tgid == 1) {
            printk("[INIT] PID 1 exited with status %u — halting\n",
                   (uint32_t)(arg1 & 0xFF));
            arch_request_shutdown();
        }

        /* C2: Reparent orphan children to init (PID 1). */
        {
            aegis_task_t *t = sched_current()->next;
            while (t != sched_current()) {
                if (t->is_user) {
                    aegis_process_t *child = (aegis_process_t *)t;
                    if (child->ppid == proc->pid)
                        child->ppid = 1;
                }
                t = t->next;
            }
        }
    }
    sched_exit();
    __builtin_unreachable();
}

/*
 * sys_clone — syscall 56
 *
 * Creates a new thread (CLONE_VM) or delegates to sys_fork (no CLONE_VM).
 *
 * flags       = clone flags | signal_number (low byte stripped)
 * child_stack = user stack pointer for new thread (0 = copy parent's)
 * ptid        = user pointer for CLONE_PARENT_SETTID
 * ctid        = user pointer for CLONE_CHILD_SETTID / CLONE_CHILD_CLEARTID
 * tls         = TLS pointer for CLONE_SETTLS
 */
uint64_t
sys_clone(syscall_frame_t *frame, uint64_t flags, uint64_t child_stack,
          uint64_t ptid, uint64_t ctid, uint64_t tls)
{
    /* Strip low byte (signal number — ignored). */
    uint32_t cl = (uint32_t)(flags & ~0xFFu);

    /* Without CLONE_VM this is a plain fork. */
    if (!(cl & CLONE_VM))
        return sys_fork(frame);

    /* ── Thread creation (CLONE_VM set) ─────────────────────────────────── */

    /* H4: Reject kernel addresses for TLS pointer. */
    if ((cl & CLONE_SETTLS) && tls >= 0xFFFF800000000000ULL)
        return (uint64_t)-(int64_t)14;  /* EFAULT */

    /* H3: Reject kernel addresses for CLONE_CHILD_CLEARTID pointer. */
    if ((cl & CLONE_CHILD_CLEARTID) && ctid >= 0xFFFF800000000000ULL)
        return (uint64_t)-(int64_t)14;  /* EFAULT */

    aegis_task_t    *parent_task = sched_current();
    if (!parent_task || !parent_task->is_user)
        return (uint64_t)-(int64_t)1;  /* EPERM */
    aegis_process_t *parent = (aegis_process_t *)parent_task;

    /* Capability gate: THREAD_CREATE required. */
    if (cap_check(parent->caps, CAP_TABLE_SIZE,
                  CAP_KIND_THREAD_CREATE, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    /* Process limit. */
    if (s_fork_count >= MAX_PROCESSES)
        return (uint64_t)(int64_t)-11;  /* -EAGAIN */

    /* 1. Allocate child PCB. */
    aegis_process_t *child = kva_alloc_pages(2);
    if (!child)
        return (uint64_t)-(int64_t)12;  /* -ENOMEM */

    /* 2. Share address space — same PML4, no page copy. */
    child->pml4_phys = parent->pml4_phys;

    /* 3. File descriptor table: share or copy. */
    if (cl & CLONE_FILES) {
        fd_table_ref(parent->fd_table);
        child->fd_table = parent->fd_table;
    } else {
        child->fd_table = fd_table_copy(parent->fd_table);
        if (!child->fd_table) {
            kva_free_pages(child, 2);
            return (uint64_t)-(int64_t)12;  /* -ENOMEM */
        }
    }

    /* 4. Copy capability tables. */
    uint32_t ci;
    for (ci = 0; ci < CAP_TABLE_SIZE; ci++)
        child->caps[ci] = parent->caps[ci];
    for (ci = 0; ci < CAP_TABLE_SIZE; ci++)
        child->exec_caps[ci] = parent->exec_caps[ci];

    /* 5. Scalar fields. */
    child->brk       = parent->brk;
    child->brk_base  = parent->brk_base;
    child->mmap_base = parent->mmap_base;
    __builtin_memcpy(child->mmap_free, parent->mmap_free,
                     parent->mmap_free_count * sizeof(mmap_free_t));
    child->mmap_free_count = parent->mmap_free_count;
    vma_share(child, parent);
    __builtin_memcpy(child->exe_path, parent->exe_path, sizeof(parent->exe_path));
    __builtin_memcpy(child->cwd, parent->cwd, sizeof(parent->cwd));
    child->pid       = proc_alloc_pid();
    child->ppid      = parent->pid;
    child->uid       = parent->uid;
    child->gid       = parent->gid;
    child->pgid      = parent->pgid;
    child->sid       = parent->sid;
    child->umask     = parent->umask;

    /* Thread-group membership. */
    if (cl & CLONE_THREAD) {
        child->tgid         = parent->tgid;
        parent->thread_count++;
    } else {
        child->tgid         = child->pid;
        child->thread_count = 1;
    }

    /* Signal state: inherit mask and dispositions; clear pending. */
    child->signal_mask     = parent->signal_mask;
    __builtin_memcpy(child->sigactions, parent->sigactions,
                     sizeof(parent->sigactions));
    child->pending_signals = 0;
    child->stop_signum     = 0;
    child->exit_status     = 0;

    /* TLS */
    if (cl & CLONE_SETTLS)
        child->task.fs_base = tls;
    else
        child->task.fs_base = parent_task->fs_base;

    /* clear_child_tid for futex-based thread join. */
    if (cl & CLONE_CHILD_CLEARTID)
        child->task.clear_child_tid = ctid;
    else
        child->task.clear_child_tid = 0;

    child->task.state       = TASK_RUNNING;
    child->task.waiting_for = 0;
    child->task.is_user     = 1;
    child->task.tid         = child->pid;
    child->task.stack_pages = 4;

    /* 6. Allocate child kernel stack (4 pages / 16 KB). */
    uint8_t *kstack = kva_alloc_pages(4);
    if (!kstack) {
        /* Security: undo all allocations made before this point.
         * fd_table was ref'd or copied at step 3, vma_share
         * incremented the parent's vma_refcount at step 5, and
         * thread_count was bumped at step 12. Leaking any of
         * these corrupts the parent's bookkeeping permanently. */
        if (cl & CLONE_THREAD)
            parent->thread_count--;
        vma_free(child);
        fd_table_unref(child->fd_table);
        kva_free_pages(child, 2);
        return (uint64_t)-(int64_t)12;  /* -ENOMEM */
    }

    /* 7. Build child initial kernel stack frame.
     *
     * Identical layout to sys_fork — a fake isr_common_stub + ctx_switch
     * frame so the child's first scheduling returns through isr_post_dispatch
     * → iretq to user space.  Only difference: when child_stack != 0, use
     * child_stack instead of frame->user_rsp for the iretq RSP slot. */
    uint64_t *sp = (uint64_t *)(kstack + 4 * 4096);
    uint64_t user_rsp = child_stack ? child_stack : FRAME_SP(frame);

#ifdef __aarch64__
    extern void fork_child_return(void);

    /* Build SAVE_ALL_EL0 frame (34 slots) for the trampoline to restore */
    sp -= 34;
    for (int fi = 0; fi < 34; fi++) sp[fi] = 0;
    for (int fi = 0; fi < 31; fi++) sp[fi] = frame->regs[fi];
    sp[0]  = 0;              /* x0 = 0 (clone returns 0 in child) */
    sp[31] = user_rsp;       /* sp_el0 */
    sp[32] = frame->elr;     /* elr_el1 (return to user) */
    sp[33] = frame->spsr;    /* spsr_el1 */

    /* ctx_switch callee-save frame: 12 slots */
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
    *--sp = (uint64_t)(uintptr_t)fork_child_return; /* x30 (lr) */
    *--sp = 0;                          /* x29 (fp) */
#else
    /* x86-64: build ISR + ctx_switch frame for isr_post_dispatch path. */

    /* CPU ring-3 interrupt frame (ss = highest address) */
    *--sp = ARCH_USER_DS;            /* ss = user data selector              */
    *--sp = user_rsp;               /* user RSP (child stack or parent's)   */
    *--sp = frame->rflags;          /* RFLAGS                               */
    *--sp = ARCH_USER_CS;            /* cs = user code selector              */
    *--sp = frame->rip;             /* RIP = resume point after clone()     */

    /* ISR stub: ISR_NOERR pushes error_code(0) then vector(0) */
    *--sp = 0;                      /* error_code                           */
    *--sp = 0;                      /* vector                               */

    /* GPRs: isr_common_stub pushes rax first (high) → r15 last (low). */
    *--sp = 0;                      /* rax = 0  (clone returns 0 in child)  */
    *--sp = 0;                      /* rbx                                  */
    *--sp = frame->rip;             /* rcx = return RIP (SYSCALL semantics) */
    *--sp = 0;                      /* rdx                                  */
    *--sp = 0;                      /* rsi                                  */
    *--sp = 0;                      /* rdi                                  */
    *--sp = 0;                      /* rbp                                  */
    *--sp = frame->r8;              /* r8                                   */
    *--sp = frame->r9;              /* r9                                   */
    *--sp = frame->r10;             /* r10                                  */
    *--sp = frame->rflags;          /* r11 = RFLAGS (SYSCALL semantics)     */
    *--sp = 0;                      /* r12                                  */
    *--sp = 0;                      /* r13                                  */
    *--sp = 0;                      /* r14                                  */
    *--sp = 0;                      /* r15                                  */

    /* CR3 slot: restored by isr_post_dispatch before iretq */
    *--sp = (uint64_t)child->pml4_phys;

    /* ctx_switch callee-save frame: ret addr + r15-r12/rbp/rbx */
    *--sp = (uint64_t)(uintptr_t)isr_post_dispatch; /* ret addr            */
    *--sp = 0;  /* rbx                                                      */
    *--sp = 0;  /* rbp                                                      */
    *--sp = 0;  /* r12                                                      */
    *--sp = 0;  /* r13                                                      */
    *--sp = 0;  /* r14                                                      */
    *--sp = 0;  /* r15  <- child->task.sp points here                       */
#endif

    child->task.sp               = (uint64_t)(uintptr_t)sp;
    child->task.stack_base       = kstack;
    child->task.kernel_stack_top = (uint64_t)(uintptr_t)(kstack + 4 * 4096);

    /* Update TSS RSP0 for parent (it remains current) */
    arch_set_kernel_stack(parent_task->kernel_stack_top);

    /* 8. Add child to run queue. */
    sched_add(&child->task);
    s_fork_count++;

    /* 9. CLONE_PARENT_SETTID: write child tid to parent's *ptid. */
    if (cl & CLONE_PARENT_SETTID) {
        if (user_ptr_valid(ptid, 4)) {
            uint32_t tid_val = child->pid;
            vmm_write_user_bytes(parent->pml4_phys, ptid,
                                 &tid_val, sizeof(tid_val));
        }
    }

    /* 10. CLONE_CHILD_SETTID: write child tid to child's *ctid.
     * Same address space (CLONE_VM), so use parent's PML4. */
    if (cl & CLONE_CHILD_SETTID) {
        uint32_t tid_val = child->pid;
        vmm_write_user_bytes(parent->pml4_phys, ctid,
                             &tid_val, sizeof(tid_val));
    }

    /* 11. CLONE_VFORK: block parent until child exits or execs. */
    if (cl & CLONE_VFORK) {
        parent_task->waiting_for = child->pid;
        sched_block();
    }

    return (uint64_t)child->pid;
}

/*
 * sys_fork — syscall 57
 *
 * Duplicates the calling process.  Returns child PID in the parent,
 * 0 in the child (via the fork_child_return SYSRET path).
 *
 * Steps:
 *   1. Allocate child PCB via kva.
 *   2. Copy parent fd table, capability table, and scalar fields.
 *   3. Create a new PML4 and deep-copy all user pages.
 *   4. Allocate a kernel stack for the child.
 *   5. Build the initial kernel stack frame so ctx_switch resumes at
 *      fork_child_return, which issues SYSRET back to user space with rax=0.
 *   6. Add child to the run queue.
 *   7. Return child PID to the parent.
 */
uint64_t
sys_fork(syscall_frame_t *frame)
{
    aegis_task_t    *parent_task = sched_current();
    if (!parent_task || !parent_task->is_user) return (uint64_t)-(int64_t)1; /* EPERM */
    aegis_process_t *parent      = (aegis_process_t *)parent_task;

    /* S10: Prevent fork bombs — cap total process count. */
    if (s_fork_count >= MAX_PROCESSES)
        return (uint64_t)(int64_t)-11;  /* -EAGAIN */

    /* 1. Allocate child PCB */
    aegis_process_t *child = kva_alloc_pages(2);
    if (!child)
        return (uint64_t)-(int64_t)12;   /* -ENOMEM */

    /* 2. Copy parent fd table (allocates new table, bumps driver refs) */
    child->fd_table = fd_table_copy(parent->fd_table);
    if (!child->fd_table) {
        kva_free_pages(child, 2);
        return (uint64_t)-(int64_t)12;   /* -ENOMEM */
    }

    uint32_t ci;
    for (ci = 0; ci < CAP_TABLE_SIZE; ci++)
        child->caps[ci] = parent->caps[ci];

    /* exec_caps are not inherited by fork — child starts with an empty exec_caps table */
    for (ci = 0; ci < CAP_TABLE_SIZE; ci++) {
        child->exec_caps[ci].kind   = CAP_KIND_NULL;
        child->exec_caps[ci].rights = 0;
    }

    child->brk             = parent->brk;
    child->brk_base        = parent->brk_base;
    child->mmap_base       = parent->mmap_base;
    __builtin_memcpy(child->mmap_free, parent->mmap_free,
                     parent->mmap_free_count * sizeof(mmap_free_t));
    child->mmap_free_count = parent->mmap_free_count;
    vma_clone(child, parent);
    __builtin_memcpy(child->exe_path, parent->exe_path, sizeof(parent->exe_path));
#ifdef __aarch64__
    /* ARM64: musl sets TPIDR_EL0 directly, not via arch_prctl.
     * Read the current TPIDR_EL0 value and save to child. */
    {
        uint64_t tpidr;
        __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tpidr));
        child->task.fs_base = tpidr;
    }
#else
    child->task.fs_base    = parent_task->fs_base;
#endif
    __builtin_memcpy(child->cwd, parent->cwd, sizeof(parent->cwd));
    child->pid             = proc_alloc_pid();
    child->tgid            = child->pid;
    child->thread_count    = 1;
    child->ppid            = parent->pid;
    child->pgid            = parent->pgid;
    child->sid             = parent->sid;
    child->uid             = parent->uid;
    child->gid             = parent->gid;
    child->umask           = parent->umask;
    child->stop_signum     = 0;
    child->exit_status     = 0;
    /* Signal state: inherit mask and dispositions; clear pending (Linux semantics) */
    child->signal_mask     = parent->signal_mask;
    __builtin_memcpy(child->sigactions, parent->sigactions, sizeof(parent->sigactions));
    child->pending_signals = 0;
    child->task.state      = TASK_RUNNING;
    child->task.waiting_for = 0;
    child->task.is_user    = 1;
    child->task.tid        = child->pid;   /* use pid as tid */
    child->task.stack_pages = 4;  /* 4 pages / 16 KB — see proc.c KSTACK_NPAGES */

    /* 3. Create child PML4 */
    child->pml4_phys = vmm_create_user_pml4();
    if (!child->pml4_phys) {
        /* C3: free fd_table + VMA allocated above */
        kva_free_pages(child->fd_table, 1);
        vma_clear(child);
        kva_free_pages(child, 2);
        return (uint64_t)-(int64_t)12;           /* -ENOMEM */
    }

    /* 4. Copy parent user pages into child PML4 */
    if (vmm_copy_user_pages(parent->pml4_phys, child->pml4_phys) != 0) {
        vmm_free_user_pml4(child->pml4_phys);
        kva_free_pages(child->fd_table, 1);
        vma_clear(child);
        kva_free_pages(child, 2);
        return (uint64_t)-(int64_t)12;           /* -ENOMEM */
    }

    /* 5. Allocate child kernel stack (4 pages / 16 KB — same as proc_spawn).
     * pipe_write_fn's 4060-byte staging buffer requires at least 4 pages;
     * see proc.c KSTACK_NPAGES comment for the full budget analysis. */
    uint8_t *kstack = kva_alloc_pages(4);
    if (!kstack) {
        vmm_free_user_pml4(child->pml4_phys);
        kva_free_pages(child->fd_table, 1);
        vma_clear(child);
        kva_free_pages(child, 2);
        return (uint64_t)-(int64_t)12;           /* -ENOMEM */
    }

    /* 6. Build child initial kernel stack frame.
     *
     * We build a complete fake isr_common_stub post-dispatch frame so the
     * child's first scheduling is identical to every subsequent one: ctx_switch
     * pops callee-saves, rets to isr_post_dispatch, pops GPRs from the
     * cpu_state_t, restores CR3 (child's PML4), and iretqs to user space.
     * This avoids the SYSRET path entirely, eliminating the stale-frame
     * register corruption that caused r12=0 / ss=0x18 crashes.
     *
     * Stack layout (low→high, child->task.sp = lowest address):
     *
     *   -- ctx_switch callee-save frame (7 slots) --
     *   [r15=0][r14=0][r13=0][r12=0][rbp=0][rbx=0]
     *   [isr_post_dispatch]      <- ret addr; ctx_switch rets here
     *
     *   -- fake isr_common_stub frame --
     *   [CR3 = child->pml4_phys] <- isr_post_dispatch pops → restores PML4
     *   [r15=0][r14=0][r13=0][r12=0][r11=rflags][r10][r9][r8]
     *   [rbp=0][rdi=0][rsi=0][rdx=0][rcx=rip][rbx=0][rax=0]  <- fork ret 0
     *   [vector=0][error_code=0]
     *   [rip][cs=ARCH_USER_CS][rflags][user_rsp][ss=ARCH_USER_DS]  <- CPU ring-3 frame
     *
     * isr_post_dispatch: pop CR3 → mov cr3 → pop r15..rax → add rsp,16 → iretq
     */
    uint64_t *sp = (uint64_t *)(kstack + 4 * 4096);

#ifdef __aarch64__
    /* ARM64 fork child frame: ctx_switch callee-saves + a trampoline that
     * restores the EL0 exception frame and ERETs to user space.
     * Build a ctx_switch frame that returns to fork_child_return
     * (implemented in proc_enter.S) which sets x0=0 and ERETs. */
    extern void fork_child_return(void);

    /* Build SAVE_ALL_EL0 frame (34 slots) for the trampoline to restore */
    sp -= 34;
    for (int fi = 0; fi < 34; fi++) sp[fi] = 0;
    /* Copy parent's saved regs into child frame */
    for (int fi = 0; fi < 31; fi++) sp[fi] = frame->regs[fi];
    sp[0]  = 0;              /* x0 = 0 (fork returns 0 in child) */
    sp[31] = frame->user_sp; /* sp_el0 */
    sp[32] = frame->elr;     /* elr_el1 (return to user) */
    sp[33] = frame->spsr;    /* spsr_el1 */

    /* ctx_switch callee-save frame: 12 slots (matching ctx_switch.S) */
    /* lr (x30) = fork_child_return, rest zeroed */
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
    *--sp = (uint64_t)(uintptr_t)fork_child_return; /* x30 (lr) */
    *--sp = 0;                          /* x29 (fp) */
#else
    /* x86-64: build ISR + ctx_switch frame for isr_post_dispatch path. */

    /* CPU ring-3 interrupt frame (ss = highest address) */
    *--sp = ARCH_USER_DS;            /* ss = user data selector              */
    *--sp = frame->user_rsp;        /* user RSP                             */
    *--sp = frame->rflags;          /* RFLAGS                               */
    *--sp = ARCH_USER_CS;            /* cs = user code selector              */
    *--sp = frame->rip;             /* RIP = resume point after fork()      */

    /* ISR stub: ISR_NOERR pushes error_code(0) then vector(0) */
    *--sp = 0;                      /* error_code                           */
    *--sp = 0;                      /* vector                               */

    /* GPRs: isr_common_stub pushes rax first (high) → r15 last (low). */
    *--sp = 0;                      /* rax = 0  (fork returns 0 in child)   */
    *--sp = 0;                      /* rbx                                  */
    *--sp = frame->rip;             /* rcx = return RIP (SYSCALL semantics) */
    *--sp = 0;                      /* rdx                                  */
    *--sp = 0;                      /* rsi                                  */
    *--sp = 0;                      /* rdi                                  */
    *--sp = 0;                      /* rbp                                  */
    *--sp = frame->r8;              /* r8                                   */
    *--sp = frame->r9;              /* r9                                   */
    *--sp = frame->r10;             /* r10                                  */
    *--sp = frame->rflags;          /* r11 = RFLAGS (SYSCALL semantics)     */
    *--sp = 0;                      /* r12                                  */
    *--sp = 0;                      /* r13                                  */
    *--sp = 0;                      /* r14                                  */
    *--sp = 0;                      /* r15                                  */

    /* CR3 slot: restored by isr_post_dispatch before iretq */
    *--sp = (uint64_t)child->pml4_phys;

    /* ctx_switch callee-save frame: ret addr + r15-r12/rbp/rbx */
    *--sp = (uint64_t)(uintptr_t)isr_post_dispatch; /* ret addr            */
    *--sp = 0;  /* rbx                                                      */
    *--sp = 0;  /* rbp                                                      */
    *--sp = 0;  /* r12                                                      */
    *--sp = 0;  /* r13                                                      */
    *--sp = 0;  /* r14                                                      */
    *--sp = 0;  /* r15  <- child->task.sp points here                       */
#endif

    child->task.sp               = (uint64_t)(uintptr_t)sp;
    child->task.stack_base       = kstack;
    child->task.kernel_stack_top = (uint64_t)(uintptr_t)(kstack + 4 * 4096);

    /* Update TSS RSP0 for parent (it remains current) */
    arch_set_kernel_stack(parent_task->kernel_stack_top);

    /* 7. Add child to run queue */
    sched_add(&child->task);
    s_fork_count++;

    /* Return child PID to parent */
    return (uint64_t)child->pid;
}

/*
 * sys_waitpid — syscall 61
 *
 * pid_arg = PID to wait for (-1 = any child)
 * wstatus_ptr = user pointer to write exit status (0 = ignored)
 * options = WNOHANG (1) = return 0 immediately if no zombie child
 *
 * Scans the run queue for a zombie child matching the request.
 * On match: writes exit status (if wstatus_ptr != 0), removes zombie from
 * run queue, frees its resources, and returns the child's PID.
 * If no zombie is found and WNOHANG is set: returns 0.
 * If no zombie is found and WNOHANG is not set: blocks until a child exits
 * (sched_block), then retries via goto.
 */
uint64_t
sys_waitpid(uint64_t pid_arg, uint64_t wstatus_ptr, uint64_t options)
{
    aegis_process_t *caller = (aegis_process_t *)sched_current();
    int32_t          pid    = (int32_t)(uint32_t)pid_arg;

retry:;
    /* Scan run queue for a matching child (stopped or zombie). */
    aegis_task_t *t = sched_current()->next;
    while (t != sched_current()) {
        if (t->is_user) {
            aegis_process_t *child = (aegis_process_t *)t;

            /* Check for stopped child (WUNTRACED) */
            if (t->state == TASK_STOPPED && (options & WUNTRACED) &&
                child->ppid == caller->pid &&
                (pid == -1 || (uint32_t)pid == child->pid)) {
                uint32_t child_pid = child->pid;
                if (wstatus_ptr) {
                    if (!user_ptr_valid(wstatus_ptr, 4))
                        return (uint64_t)-(int64_t)14; /* EFAULT */
                    /* WIFSTOPPED encoding: (signum << 8) | 0x7f */
                    uint32_t wstatus_val = (child->stop_signum << 8) | 0x7fU;
                    copy_to_user((void *)(uintptr_t)wstatus_ptr, &wstatus_val, 4);
                }
                /* Clear stop_signum so repeated waitpid doesn't re-report */
                child->stop_signum = 0;
                sched_current()->waiting_for = 0;
                return (uint64_t)child_pid;
            }

            /* Check for zombie child (normal reap) */
            if (t->state == TASK_ZOMBIE &&
                child->ppid == caller->pid &&
                (pid == -1 || (uint32_t)pid == child->pid)) {
                /* Found a zombie to reap. */
                uint32_t child_pid = child->pid;
                uint64_t status    = child->exit_status & 0xFF;

                /* Write exit status to user if requested. */
                if (wstatus_ptr) {
                    if (!user_ptr_valid(wstatus_ptr, 4)) return (uint64_t)-(int64_t)14; /* EFAULT */
                    uint32_t wstatus_val = (uint32_t)(status << 8);
                    copy_to_user((void *)(uintptr_t)wstatus_ptr,
                                 &wstatus_val, 4);
                }

                /* Remove zombie from run queue (find predecessor). */
                aegis_task_t *prev = t;
                while (prev->next != t) prev = prev->next;
                prev->next = t->next;

                /* Free zombie resources. */
                kva_free_pages(child->task.stack_base, child->task.stack_pages);

                /* Security: threads (tgid != pid) share the leader's PML4.
                 * Freeing it here would destroy the address space of all
                 * sibling threads still running.  Only free for the group
                 * leader (tgid == pid) or standalone processes. */
                if (child->tgid == child->pid) {
                    vmm_free_user_pml4(child->pml4_phys);
                }
                vma_free(child);
                kva_free_pages(child, 2);

                /* Clear waiting_for on the caller — no longer blocked. */
                sched_current()->waiting_for = 0;

                return (uint64_t)child_pid;
            }
        }
        t = t->next;
    }

    /* No zombie found. */
    if (options & WNOHANG) return 0;

    /* Block until a child changes state, then retry. */
    sched_current()->waiting_for = (pid == -1) ? 0 : (uint32_t)pid;
    sched_block();
    goto retry;
}
