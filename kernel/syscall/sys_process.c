/* sys_process.c — Process lifecycle syscalls: exit, fork, execve, waitpid */
#include "sys_impl.h"
#include "futex.h"
#include "vma.h"

#define MAX_PROCESSES 64
static uint32_t s_fork_count = 1;  /* starts at 1 for init */

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
 * arg1 = exit code (ignored for Phase 5)
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

        if (proc->pid == 1) {
            printk("[INIT] PID 1 exited with status %u — halting\n",
                   (uint32_t)(arg1 & 0xFF));
            arch_request_shutdown();
        }
    }
    sched_exit();
    __builtin_unreachable();
}

/* ── Stubs ─────────────────────────────────────────────────────────────────
 * musl startup calls these; they do not require real implementations in
 * Phase 14 with a single short-lived process.
 */
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

        if (proc->pid == 1 || proc->tgid == 1) {
            printk("[INIT] PID 1 exited with status %u — halting\n",
                   (uint32_t)(arg1 & 0xFF));
            arch_request_shutdown();
        }
    }
    sched_exit();
    __builtin_unreachable();
}

uint64_t
sys_getpid(void)
{
    aegis_task_t *task = sched_current();
    if (!task->is_user) return 0;
    return (uint64_t)((aegis_process_t *)task)->tgid;
}

uint64_t
sys_gettid(void)
{
    aegis_task_t *task = sched_current();
    if (!task->is_user) return 0;
    return (uint64_t)((aegis_process_t *)task)->pid;
}

/*
 * sys_getppid — syscall 110
 *
 * Returns the parent PID of the calling process.
 * No capability gate — a process may always query its own parent.
 */
uint64_t
sys_getppid(void)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    return (uint64_t)proc->ppid;
}

uint64_t
sys_set_tid_address(uint64_t arg1)
{
    aegis_task_t *task = sched_current();
    task->clear_child_tid = arg1;
    if (!task->is_user) return 1;
    return (uint64_t)((aegis_process_t *)task)->pid;
}

uint64_t sys_set_robust_list(uint64_t a, uint64_t b)
{
    (void)a; (void)b;
    return 0;
}

/*
 * sys_arch_prctl — syscall 158
 *
 * arg1 = code
 * arg2 = addr
 *
 * ARCH_SET_FS (0x1002): set FS.base to addr. Writes IA32_FS_BASE MSR
 *   and saves to proc->fs_base.
 * ARCH_GET_FS (0x1003): write current fs_base to *addr.
 * All other codes: return -EINVAL.
 */
uint64_t
sys_arch_prctl(uint64_t arg1, uint64_t arg2)
{
    if (arg1 == ARCH_SET_FS) {
        sched_current()->fs_base = arg2;
        arch_set_fs_base(arg2);
        return 0;
    }
    if (arg1 == ARCH_GET_FS) {
        if (!user_ptr_valid(arg2, sizeof(uint64_t)))
            return (uint64_t)-(int64_t)14;   /* EFAULT */
        copy_to_user((void *)(uintptr_t)arg2, &sched_current()->fs_base,
                     sizeof(uint64_t));
        return 0;
    }
    return (uint64_t)-(int64_t)22;   /* EINVAL */
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
    aegis_process_t *child = kva_alloc_pages(1);
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
            kva_free_pages(child, 1);
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
        kva_free_pages(child, 1);
        return (uint64_t)-(int64_t)12;  /* -ENOMEM */
    }

    /* 7. Build child initial kernel stack frame.
     *
     * Identical layout to sys_fork — a fake isr_common_stub + ctx_switch
     * frame so the child's first scheduling returns through isr_post_dispatch
     * → iretq to user space.  Only difference: when child_stack != 0, use
     * child_stack instead of frame->user_rsp for the iretq RSP slot. */
    uint64_t *sp = (uint64_t *)(kstack + 4 * 4096);
    uint64_t user_rsp = child_stack ? child_stack : frame->user_rsp;

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
    *--sp = 0x1B;                   /* ss = user data selector              */
    *--sp = user_rsp;               /* user RSP (child stack or parent's)   */
    *--sp = frame->rflags;          /* RFLAGS                               */
    *--sp = 0x23;                   /* cs = user code selector              */
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
        uint32_t tid_val = child->pid;
        vmm_write_user_bytes(parent->pml4_phys, ptid,
                             &tid_val, sizeof(tid_val));
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
    aegis_process_t *child = kva_alloc_pages(1);
    if (!child)
        return (uint64_t)-(int64_t)12;   /* -ENOMEM */

    /* 2. Copy parent fd table (allocates new table, bumps driver refs) */
    child->fd_table = fd_table_copy(parent->fd_table);
    if (!child->fd_table) {
        kva_free_pages(child, 1);
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
        kva_free_pages(child, 1);
        return (uint64_t)-(int64_t)12;           /* -ENOMEM */
    }

    /* 4. Copy parent user pages into child PML4 */
    if (vmm_copy_user_pages(parent->pml4_phys, child->pml4_phys) != 0) {
        vmm_free_user_pml4(child->pml4_phys);
        kva_free_pages(child, 1);
        return (uint64_t)-(int64_t)12;           /* -ENOMEM */
    }

    /* 5. Allocate child kernel stack (4 pages / 16 KB — same as proc_spawn).
     * pipe_write_fn's 4060-byte staging buffer requires at least 4 pages;
     * see proc.c KSTACK_NPAGES comment for the full budget analysis. */
    uint8_t *kstack = kva_alloc_pages(4);
    if (!kstack) {
        vmm_free_user_pml4(child->pml4_phys);
        kva_free_pages(child, 1);
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
     *   [rip][cs=0x23][rflags][user_rsp][ss=0x1B]  <- CPU ring-3 frame
     *
     * isr_post_dispatch: pop CR3 → mov cr3 → pop r15..rax → add rsp,16 → iretq
     */
    uint64_t *sp = (uint64_t *)(kstack + 4 * 4096);

#ifdef __aarch64__
    /* ARM64 fork child frame: ctx_switch callee-saves + a trampoline that
     * restores the EL0 exception frame and ERETs to user space.
     * For now, build a ctx_switch frame that returns to fork_child_return
     * which sets x0=0 (child return) and returns to user via ERET.
     * TODO: implement fork_child_return trampoline in vectors.S */
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
    *--sp = 0x1B;                   /* ss = user data selector              */
    *--sp = frame->user_rsp;        /* user RSP                             */
    *--sp = frame->rflags;          /* RFLAGS                               */
    *--sp = 0x23;                   /* cs = user code selector              */
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
                vmm_free_user_pml4(child->pml4_phys);
                vma_free(child);
                kva_free_pages(child, 1);

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

/*
 * sys_execve — syscall 59
 *
 * arg1 = user pointer to null-terminated path
 * arg2 = user pointer to null-terminated argv[] array (NULL-terminated)
 * arg3 = user pointer to envp[] (ignored; Phase 15 always passes empty env)
 *
 * Replaces the calling process image in place:
 *   1. Copy path and argv from user.
 *   2. Look up path in initrd.
 *   3. Free all user leaf pages; keep PML4 structure.
 *   4. Reset brk/mmap_base/fs_base.
 *   5. Load new ELF into the existing PML4.
 *   6. Allocate a fresh user stack (4 pages / 16 KB).
 *   7. Build x86-64 SysV ABI initial stack: argc, argv ptrs, NULL, envp
 *      NULL, auxv (AT_PHDR, AT_PHNUM, AT_PAGESZ, AT_ENTRY, AT_NULL).
 *   8. Redirect SYSRET to new entry point.
 *
 * Stack layout on return:
 *   SP → argc
 *        argv[0] ... argv[argc-1]
 *        NULL (argv terminator)
 *        NULL (envp terminator)
 *        AT_PHDR  / phdr_va
 *        AT_PHNUM / phdr_count
 *        AT_PAGESZ / 4096
 *        AT_ENTRY / er.entry
 *        AT_NULL  / 0
 *
 * Alignment: RSP % 16 == 8 on entry to _start, per SysV ABI.
 *
 * argv_bufs lives in a kva-allocated buffer (not on the kernel stack)
 * because argv_bufs[64][256] = 16 KB exceeds the per-process kernel stack.
 */
uint64_t
sys_execve(syscall_frame_t *frame,
           uint64_t path_uptr, uint64_t argv_uptr, uint64_t envp_uptr)
{
    (void)envp_uptr;  /* Phase 15: empty environment */

    aegis_process_t *proc = (aegis_process_t *)sched_current();
    /* Allocate argv working area from kva — too large for kernel stack. */
    execve_argbuf_t *abuf = kva_alloc_pages(EXECVE_ARGBUF_PAGES);
    if (!abuf)
        return (uint64_t)-(int64_t)12;  /* ENOMEM */

    uint64_t ret = 0;  /* overwritten on error; 0 = success */
    /* For ext2-backed binaries: kva buffer holding the ELF image. */
    void    *ext2_buf   = (void *)0;
    uint64_t ext2_pages = 0;

    /* 1. Copy path from user (<=255 bytes) */
    char path[256];
    if (!user_ptr_valid(path_uptr, 1)) { ret = (uint64_t)-(int64_t)14; goto done; }
    {
        uint64_t i;
        for (i = 0; i < sizeof(path) - 1; i++) {
            if (!user_ptr_valid(path_uptr + i, 1))
                { ret = (uint64_t)-(int64_t)14; goto done; }
            char c;
            copy_from_user(&c, (const void *)(uintptr_t)(path_uptr + i), 1);
            path[i] = c;
            if (c == '\0') break;
        }
        path[sizeof(path) - 1] = '\0';
    }

    /* 2. Copy argv from user (<=64 entries, each <=255 bytes) */
    {
        int argc = 0;
        uint64_t ptr_addr = argv_uptr;
        while (argc < 64) {
            if (!user_ptr_valid(ptr_addr, 8))
                { ret = (uint64_t)-(int64_t)14; goto done; }
            uint64_t str_ptr;
            copy_from_user(&str_ptr,
                           (const void *)(uintptr_t)ptr_addr, 8);
            if (!str_ptr) break;  /* NULL terminator */
            {
                uint64_t i;
                for (i = 0; i < 255; i++) {
                    if (!user_ptr_valid(str_ptr + i, 1))
                        { ret = (uint64_t)-(int64_t)14; goto done; }
                    char c;
                    copy_from_user(&c,
                        (const void *)(uintptr_t)(str_ptr + i), 1);
                    abuf->argv_bufs[argc][i] = c;
                    if (c == '\0') break;
                }
            }
            abuf->argv_bufs[argc][255] = '\0';
            abuf->argv_ptrs[argc] = abuf->argv_bufs[argc];
            argc++;
            ptr_addr += 8;
        }
        abuf->argv_ptrs[argc] = (char *)0;
        /* argc is now a block-local — capture it for the rest of the function */
        {
    int argc2 = argc;

    /* 3. Look up binary: initrd first, then VFS (ext2 on nvme0p1). */
    vfs_file_t f;
    const uint8_t *elf_data;
    uint64_t       elf_size;
    if (initrd_open(path, &f) == 0) {
        elf_data = (const uint8_t *)initrd_get_data(&f);
        elf_size = (uint64_t)initrd_get_size(&f);
    } else {
        vfs_file_t vf;
        int vr = vfs_open(path, 0, &vf);
        if (vr != 0)
            { ret = (uint64_t)-(int64_t)2; goto done; }  /* ENOENT */
        if (vf.size == 0)
            { ret = (uint64_t)-(int64_t)8; goto done; }  /* ENOEXEC */
        ext2_pages = (vf.size + 4095ULL) / 4096ULL;
        ext2_buf = kva_alloc_pages(ext2_pages);
        if (!ext2_buf)
            { ret = (uint64_t)-(int64_t)12; goto done; }  /* ENOMEM */
        int rr = vf.ops->read(vf.priv, ext2_buf, 0, vf.size);
        if (rr < 0)
            { ret = (uint64_t)-(int64_t)5; goto done; }   /* EIO */
        elf_data = (const uint8_t *)ext2_buf;
        elf_size = vf.size;
    }

    /* 4. Free all user leaf pages; reuse PML4 and page-table structure */
    vmm_free_user_pages(proc->pml4_phys);
    vmm_switch_to(proc->pml4_phys);   /* reload CR3 to flush stale TLBs */

    /* 5. Reset heap/mmap/TLS state */
    proc->brk       = 0;
    proc->mmap_base = 0x0000700000000000ULL;
    proc->mmap_free_count = 0;
    vma_clear(proc);
    {
        uint64_t pi;
        for (pi = 0; pi < sizeof(proc->exe_path) - 1 && path[pi]; pi++)
            proc->exe_path[pi] = path[pi];
        proc->exe_path[pi] = '\0';
    }
    proc->task.fs_base = 0;

    /* Reset capability table to baseline on exec — exec is a capability boundary.
     * Login's AUTH/SETUID caps must not propagate to the exec'd shell. */
    {
        uint32_t ci;
        for (ci = 0; ci < CAP_TABLE_SIZE; ci++) {
            proc->caps[ci].kind   = CAP_KIND_NULL;
            proc->caps[ci].rights = 0;
        }
        cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_VFS_OPEN,   CAP_RIGHTS_READ);
        cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_VFS_WRITE,  CAP_RIGHTS_WRITE);
        cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_VFS_READ,   CAP_RIGHTS_READ);
        cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_NET_SOCKET, CAP_RIGHTS_READ);
        cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_THREAD_CREATE, CAP_RIGHTS_READ);
        cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_PROC_READ, CAP_RIGHTS_READ);

        /* Apply pre-registered exec caps, then zero them (consumed on exec). */
        {
            uint32_t ci;
            for (ci = 0; ci < CAP_TABLE_SIZE; ci++) {
                if (proc->exec_caps[ci].kind != CAP_KIND_NULL) {
                    cap_grant(proc->caps, CAP_TABLE_SIZE,
                              proc->exec_caps[ci].kind,
                              proc->exec_caps[ci].rights);
                    proc->exec_caps[ci].kind   = CAP_KIND_NULL;
                    proc->exec_caps[ci].rights = 0;
                }
            }
        }
    }

    /* 6. Load new ELF */
    elf_load_result_t er;
    if (elf_load(proc->pml4_phys, elf_data, (size_t)elf_size, &er) != 0)
        { ret = (uint64_t)-(int64_t)8; goto done; }  /* ENOEXEC */
    /* Free ext2 buffer immediately after ELF is loaded (pages already mapped). */
    if (ext2_buf) {
        kva_free_pages(ext2_buf, ext2_pages);
        ext2_buf   = (void *)0;
        ext2_pages = 0;
    }
    proc->brk = er.brk;

    /* 7. Allocate + map 4 user stack pages (16 KB) */
    {
        uint64_t pn;
        for (pn = 0; pn < USER_STACK_NPAGES; pn++) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) {
                /* Free all user pages already mapped (ELF + partial stack) */
                vmm_free_user_pages(proc->pml4_phys);
                ret = (uint64_t)-(int64_t)12;
                goto done;  /* ENOMEM */
            }
            vmm_zero_page(phys);
            vmm_map_user_page(proc->pml4_phys,
                              USER_STACK_BASE_EXEC + pn * 4096ULL, phys,
                              VMM_FLAG_PRESENT | VMM_FLAG_USER |
                              VMM_FLAG_WRITABLE);
        }
    }

    /* Record user stack VMA */
    vma_insert(proc, USER_STACK_BASE_EXEC, USER_STACK_NPAGES * 4096ULL,
               0x01 | 0x02, VMA_STACK);

    /* 8. Build ABI initial stack at USER_STACK_TOP_EXEC.
     *
     * Pack argv strings first (working downward from top), then write the
     * pointer table below.  Stack pointer must satisfy RSP % 16 == 8 at
     * _start entry per the x86-64 SysV ABI.
     */
    uint64_t sp_va = USER_STACK_TOP_EXEC;

    /* 8a. Write argv strings onto the stack, recording their VAs */
    {
        int i;
        for (i = argc2 - 1; i >= 0; i--) {
            uint64_t slen = 0;
            while (abuf->argv_ptrs[i][slen]) slen++;
            slen++;  /* include null terminator */
            if (sp_va - slen < USER_STACK_BASE_EXEC)
                { ret = (uint64_t)-(int64_t)7; goto done; }  /* E2BIG */
            sp_va -= slen;
            if (vmm_write_user_bytes(proc->pml4_phys, sp_va,
                                     abuf->argv_ptrs[i], slen) != 0)
                { ret = (uint64_t)-(int64_t)14; goto done; }  /* EFAULT */
            abuf->str_ptrs[i] = sp_va;
        }
    }

    /* Write 16 bytes of random data for AT_RANDOM, then align.
     * musl 1.2.x reads AT_RANDOM from auxv to seed its internal PRNG;
     * without it, musl dereferences a null pointer during CRT init. */
    sp_va -= 16;
    {
        uint8_t rnd[16];
        random_get_bytes(rnd, 16);
        if (vmm_write_user_bytes(proc->pml4_phys, sp_va, rnd, 16) != 0)
            { ret = (uint64_t)-(int64_t)14; goto done; }
    }
    uint64_t at_random_va = sp_va;

    /* Align sp_va to 8 bytes for the pointer table */
    sp_va &= ~7ULL;

    /* Count pointer table entries:
     *   1 (argc)
     * + argc (argv pointers)
     * + 1 (argv NULL)
     * + 1 (envp NULL)
     * + 12 (6 auxv key/value pairs: PHDR, PHNUM, PAGESZ, ENTRY, RANDOM, NULL)
     * = argc + 15 qwords
     */
    {
    uint64_t table_qwords = (uint64_t)(argc2 + 15);
    uint64_t table_bytes  = table_qwords * 8ULL;

    /* Ensure RSP % 16 == 8 on entry to _start */
    sp_va -= table_bytes;
    if ((sp_va % 16) != 8)
        sp_va -= 8;
    if (sp_va < USER_STACK_BASE_EXEC)
        { ret = (uint64_t)-(int64_t)7; goto done; }  /* E2BIG */

    /* 8b. Write the pointer table */
    {
        int i;
        uint64_t wp = sp_va;

        if (vmm_write_user_u64(proc->pml4_phys, wp,
                               (uint64_t)argc2) != 0)
            { ret = (uint64_t)-(int64_t)14; goto done; }
        wp += 8;

        for (i = 0; i < argc2; i++) {
            if (vmm_write_user_u64(proc->pml4_phys, wp,
                                   abuf->str_ptrs[i]) != 0)
                { ret = (uint64_t)-(int64_t)14; goto done; }
            wp += 8;
        }

        /* argv NULL terminator */
        if (vmm_write_user_u64(proc->pml4_phys, wp, 0ULL) != 0)
            { ret = (uint64_t)-(int64_t)14; goto done; }
        wp += 8;

        /* envp NULL (empty environment) */
        if (vmm_write_user_u64(proc->pml4_phys, wp, 0ULL) != 0)
            { ret = (uint64_t)-(int64_t)14; goto done; }
        wp += 8;

        /* auxv: AT_PHDR */
        if (vmm_write_user_u64(proc->pml4_phys, wp, 3ULL) != 0)
            { ret = (uint64_t)-(int64_t)14; goto done; }
        wp += 8;
        if (vmm_write_user_u64(proc->pml4_phys, wp, er.phdr_va) != 0)
            { ret = (uint64_t)-(int64_t)14; goto done; }
        wp += 8;

        /* auxv: AT_PHNUM */
        if (vmm_write_user_u64(proc->pml4_phys, wp, 5ULL) != 0)
            { ret = (uint64_t)-(int64_t)14; goto done; }
        wp += 8;
        if (vmm_write_user_u64(proc->pml4_phys, wp,
                               (uint64_t)er.phdr_count) != 0)
            { ret = (uint64_t)-(int64_t)14; goto done; }
        wp += 8;

        /* auxv: AT_PAGESZ */
        if (vmm_write_user_u64(proc->pml4_phys, wp, 6ULL) != 0)
            { ret = (uint64_t)-(int64_t)14; goto done; }
        wp += 8;
        if (vmm_write_user_u64(proc->pml4_phys, wp, 4096ULL) != 0)
            { ret = (uint64_t)-(int64_t)14; goto done; }
        wp += 8;

        /* auxv: AT_ENTRY */
        if (vmm_write_user_u64(proc->pml4_phys, wp, 9ULL) != 0)
            { ret = (uint64_t)-(int64_t)14; goto done; }
        wp += 8;
        if (vmm_write_user_u64(proc->pml4_phys, wp, er.entry) != 0)
            { ret = (uint64_t)-(int64_t)14; goto done; }
        wp += 8;

        /* auxv: AT_RANDOM — pointer to 16 random bytes on user stack */
        if (vmm_write_user_u64(proc->pml4_phys, wp, 25ULL) != 0)
            { ret = (uint64_t)-(int64_t)14; goto done; }
        wp += 8;
        if (vmm_write_user_u64(proc->pml4_phys, wp, at_random_va) != 0)
            { ret = (uint64_t)-(int64_t)14; goto done; }
        wp += 8;

        /* auxv: AT_NULL (end sentinel) */
        if (vmm_write_user_u64(proc->pml4_phys, wp, 0ULL) != 0)
            { ret = (uint64_t)-(int64_t)14; goto done; }
        wp += 8;
        if (vmm_write_user_u64(proc->pml4_phys, wp, 0ULL) != 0)
            { ret = (uint64_t)-(int64_t)14; goto done; }
    }
    } /* table_qwords/table_bytes scope */

    /* 9. Close all O_CLOEXEC file descriptors before loading new image */
    {
        int cfd;
        for (cfd = 0; cfd < PROC_MAX_FDS; cfd++) {
            if (proc->fd_table->fds[cfd].ops &&
                (proc->fd_table->fds[cfd].flags & VFS_FD_CLOEXEC)) {
                proc->fd_table->fds[cfd].ops->close(proc->fd_table->fds[cfd].priv);
                __builtin_memset(&proc->fd_table->fds[cfd], 0, sizeof(vfs_file_t));
            }
        }
    }

    /* 10. Redirect return to new ELF entry point */
    FRAME_IP(frame) = er.entry;
    FRAME_SP(frame) = sp_va;
    /* ret = 0 (success) */
        } /* argc2 scope */
    } /* argc scope */

done:
    if (ext2_buf) kva_free_pages(ext2_buf, ext2_pages);
    kva_free_pages(abuf, EXECVE_ARGBUF_PAGES);
    return ret;
}

/* sys_setpgid — syscall 109: set pgid of process pid to pgid.
 * Aegis policy: may only set pgid = pid (form own singleton group).
 * pid=0 means current process. pgid=0 means use target's pid. */
uint64_t
sys_setpgid(uint64_t pid_arg, uint64_t pgid_arg)
{
    aegis_process_t *caller = (aegis_process_t *)sched_current();
    uint32_t pid  = (uint32_t)pid_arg;
    uint32_t pgid = (uint32_t)pgid_arg;

    aegis_process_t *target = caller;
    if (pid != 0) {
        target = (void *)0;
        aegis_task_t *t = sched_current()->next;
        while (t != sched_current()) {
            if (t->is_user) {
                aegis_process_t *p = (aegis_process_t *)t;
                if (p->pid == pid) {
                    target = p;
                    break;
                }
            }
            t = t->next;
        }
        if (!target)
            return (uint64_t)-(int64_t)3; /* ESRCH */
    }

    if (pgid == 0)
        pgid = target->pid;

    /* Aegis policy: may only form own singleton group */
    if (pgid != target->pid)
        return (uint64_t)-(int64_t)1; /* EPERM */

    target->pgid = pgid;
    return 0;
}

/* sys_getpgrp — syscall 111: return current->pgid */
uint64_t
sys_getpgrp(void)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    return (uint64_t)proc->pgid;
}

/* sys_setsid — syscall 112: set pgid = pid; return pid.
 * No real session object in Phase 25.5. */
uint64_t
sys_setsid(void)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    proc->pgid = proc->pid;
    return (uint64_t)proc->pid;
}

/* sys_getpgid — syscall 121: return pgid of process pid (0 = self). */
uint64_t
sys_getpgid(uint64_t pid_arg)
{
    aegis_process_t *caller = (aegis_process_t *)sched_current();
    uint32_t pid = (uint32_t)pid_arg;
    if (pid == 0)
        return (uint64_t)caller->pgid;

    aegis_task_t *t = sched_current()->next;
    while (t != sched_current()) {
        if (t->is_user) {
            aegis_process_t *p = (aegis_process_t *)t;
            if (p->pid == pid)
                return (uint64_t)p->pgid;
        }
        t = t->next;
    }
    return (uint64_t)-(int64_t)3; /* ESRCH */
}

/* sys_umask — syscall 95: set file creation mask; return previous value.
 * Not yet wired to ext2 create (Phase 26). */
uint64_t
sys_umask(uint64_t mask)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    uint32_t old = proc->umask;
    proc->umask  = (uint32_t)(mask & 0777U);
    return (uint64_t)old;
}

/* sys_getrlimit — syscall 97: return {RLIM_INFINITY, RLIM_INFINITY}.
 * struct rlimit = {uint64_t rlim_cur, rlim_max} = 16 bytes. */
uint64_t
sys_getrlimit(uint64_t resource, uint64_t rlim_ptr)
{
    (void)resource;
    if (!user_ptr_valid(rlim_ptr, 16))
        return (uint64_t)-(int64_t)14; /* EFAULT */
    uint64_t inf[2] = { ~0ULL, ~0ULL };
    copy_to_user((void *)(uintptr_t)rlim_ptr, inf, 16);
    return 0;
}

/*
 * sys_uname — syscall 63
 * arg1 = user pointer to struct utsname (6 x 65-byte char arrays).
 * Returns kernel identity strings; oksh uses these for $HOSTNAME and PS1.
 */
uint64_t
sys_uname(uint64_t buf_uptr)
{
    /* struct utsname: sysname[65] nodename[65] release[65]
     *                 version[65] machine[65] domainname[65] */
    char uts[6 * 65];
    if (!user_ptr_valid(buf_uptr, sizeof(uts)))
        return (uint64_t)-(int64_t)14; /* EFAULT */
    __builtin_memset(uts, 0, sizeof(uts));
    __builtin_memcpy(uts + 0*65,  "Aegis",   5); /* sysname    */
    __builtin_memcpy(uts + 1*65,  "aegis",   5); /* nodename   */
    __builtin_memcpy(uts + 2*65,  "1.0.0",   5); /* release    */
    __builtin_memcpy(uts + 3*65,  "#1",      2); /* version    */
#ifdef __aarch64__
    __builtin_memcpy(uts + 4*65,  "aarch64", 7); /* machine    */
#else
    __builtin_memcpy(uts + 4*65,  "x86_64",  6); /* machine    */
#endif
    __builtin_memcpy(uts + 5*65,  "(none)",  6); /* domainname */
    copy_to_user((void *)(uintptr_t)buf_uptr, uts, sizeof(uts));
    return 0;
}

/*
 * sys_setuid — syscall 105
 * Phase 25.7: gate uses CAP_KIND_SETUID — uid=0 has no ambient authority.
 */
uint64_t
sys_setuid(uint64_t uid_arg)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_SETUID, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)13; /* EACCES */
    proc->uid = (uint32_t)uid_arg;
    return 0;
}

/*
 * sys_setgid — syscall 106
 * Phase 25.7: gate uses CAP_KIND_SETUID — uid=0 has no ambient authority.
 */
uint64_t
sys_setgid(uint64_t gid_arg)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_SETUID, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)13; /* EACCES */
    proc->gid = (uint32_t)gid_arg;
    return 0;
}

/*
 * sys_cap_grant_exec — syscall 361
 *
 * Pre-registers a capability in exec_caps[].  When the calling process
 * subsequently calls sys_execve, the exec_caps are applied to the new
 * image's cap table after the baseline (VFS_OPEN/READ/WRITE) is set,
 * then exec_caps[] is zeroed.  Allows a parent to delegate extra caps
 * (e.g. CAP_KIND_AUTH) to a child that will exec a service binary.
 *
 * Requires: CAP_KIND_CAP_GRANT with CAP_RIGHTS_READ in caller's cap table.
 */
uint64_t
sys_cap_grant_exec(uint64_t kind_arg, uint64_t rights_arg)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_CAP_GRANT, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)ENOCAP;
    uint32_t kind   = (uint32_t)kind_arg;
    uint32_t rights = (uint32_t)rights_arg;
    if (kind == CAP_KIND_NULL || kind >= 16u)
        return (uint64_t)-(int64_t)22; /* EINVAL */
    int r = cap_grant(proc->exec_caps, CAP_TABLE_SIZE, kind, rights);
    if (r < 0) return (uint64_t)-(int64_t)12; /* ENOMEM/table full */
    return 0;
}
