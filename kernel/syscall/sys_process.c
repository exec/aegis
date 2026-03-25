/* sys_process.c — Process lifecycle syscalls: exit, fork, execve, waitpid */
#include "sys_impl.h"

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
    if (sched_current()->is_user) {
        aegis_process_t *proc = (aegis_process_t *)sched_current();
        proc->exit_status = arg1 & 0xFF;
    }
    sched_exit();
    __builtin_unreachable();
}

uint64_t
sys_getpid(void)
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

uint64_t sys_set_tid_address(uint64_t arg1) { (void)arg1; return 1; }

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
    aegis_process_t *proc = (aegis_process_t *)sched_current();

    if (arg1 == ARCH_SET_FS) {
        proc->fs_base = arg2;
        arch_set_fs_base(arg2);
        return 0;
    }
    if (arg1 == ARCH_GET_FS) {
        if (!user_ptr_valid(arg2, sizeof(uint64_t)))
            return (uint64_t)-(int64_t)14;   /* EFAULT */
        copy_to_user((void *)(uintptr_t)arg2, &proc->fs_base,
                     sizeof(uint64_t));
        return 0;
    }
    return (uint64_t)-(int64_t)22;   /* EINVAL */
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


    /* 1. Allocate child PCB */
    aegis_process_t *child = kva_alloc_pages(1);
    if (!child)
        return (uint64_t)-(int64_t)12;   /* -ENOMEM */

    /* 2. Copy parent fields */
    uint32_t ci;
    for (ci = 0; ci < PROC_MAX_FDS; ci++)
        child->fds[ci] = parent->fds[ci];

    /* Call dup hooks after the full fd table is copied.
     * Two-pass ordering is required: copy all fds first (struct copy by value,
     * no ref bumps), then bump all refs. If we bumped during the copy loop,
     * an OOM failure midway would leave ref counts inconsistent.
     * The child now holds an additional reference to every open fd. */
    for (ci = 0; ci < PROC_MAX_FDS; ci++) {
        if (child->fds[ci].ops && child->fds[ci].ops->dup)
            child->fds[ci].ops->dup(child->fds[ci].priv);
    }

    for (ci = 0; ci < CAP_TABLE_SIZE; ci++)
        child->caps[ci] = parent->caps[ci];
    child->brk             = parent->brk;
    child->mmap_base       = parent->mmap_base;
    child->fs_base         = parent->fs_base;
    __builtin_memcpy(child->cwd, parent->cwd, sizeof(parent->cwd));
    child->pid             = proc_alloc_pid();
    child->ppid            = parent->pid;
    child->pgid            = parent->pgid;
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
     * Stack layout (low→high, child->task.rsp = lowest address):
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

    /* CPU ring-3 interrupt frame (ss = highest address) */
    *--sp = 0x1B;                   /* ss = user data selector              */
    *--sp = frame->user_rsp;        /* user RSP                             */
    *--sp = frame->rflags;          /* RFLAGS                               */
    *--sp = 0x23;                   /* cs = user code selector              */
    *--sp = frame->rip;             /* RIP = resume point after fork()      */

    /* ISR stub: ISR_NOERR pushes error_code(0) then vector(0) */
    *--sp = 0;                      /* error_code                           */
    *--sp = 0;                      /* vector                               */

    /* GPRs: isr_common_stub pushes rax first (high) → r15 last (low).
     * We build in reverse: r15 first (*--sp) → rax last. */
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
    *--sp = 0;  /* r15  <- child->task.rsp points here                      */

    child->task.rsp              = (uint64_t)(uintptr_t)sp;
    child->task.stack_base       = kstack;
    child->task.kernel_stack_top = (uint64_t)(uintptr_t)(kstack + 4 * 4096);

    /* Update TSS RSP0 for parent (it remains current) */
    arch_set_kernel_stack(parent_task->kernel_stack_top);

    /* 7. Add child to run queue */
    sched_add(&child->task);

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
    /* Scan run queue for a zombie child matching the request. */
    aegis_task_t *t = sched_current()->next;
    while (t != sched_current()) {
        if (t->is_user && t->state == TASK_ZOMBIE) {
            aegis_process_t *child = (aegis_process_t *)t;
            if (child->ppid == caller->pid &&
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

    /* 3. Look up path in initrd */
    vfs_file_t f;
    if (initrd_open(path, &f) != 0)
        { ret = (uint64_t)-(int64_t)2; goto done; }  /* ENOENT */

    /* 4. Free all user leaf pages; reuse PML4 and page-table structure */
    vmm_free_user_pages(proc->pml4_phys);
    vmm_switch_to(proc->pml4_phys);   /* reload CR3 to flush stale TLBs */

    /* 5. Reset heap/mmap/TLS state */
    proc->brk       = 0;
    proc->mmap_base = 0x0000700000000000ULL;
    proc->fs_base   = 0;

    /* 6. Load new ELF */
    elf_load_result_t er;
    if (elf_load(proc->pml4_phys,
                 (const uint8_t *)initrd_get_data(&f),
                 (size_t)initrd_get_size(&f), &er) != 0)
        { ret = (uint64_t)-(int64_t)8; goto done; }  /* ENOEXEC */
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

    /* Align sp_va to 8 bytes for the pointer table */
    sp_va &= ~7ULL;

    /* Count pointer table entries:
     *   1 (argc)
     * + argc (argv pointers)
     * + 1 (argv NULL)
     * + 1 (envp NULL)
     * + 10 (5 auxv key/value pairs)
     * = argc + 13 qwords
     */
    {
    uint64_t table_qwords = (uint64_t)(argc2 + 13);
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
            if (proc->fds[cfd].ops &&
                (proc->fds[cfd].flags & VFS_FD_CLOEXEC)) {
                proc->fds[cfd].ops->close(proc->fds[cfd].priv);
                __builtin_memset(&proc->fds[cfd], 0, sizeof(vfs_file_t));
            }
        }
    }

    /* 10. Redirect SYSRET to new ELF entry point */
    frame->rip      = er.entry;
    frame->user_rsp = sp_va;
    /* ret = 0 (success) */
        } /* argc2 scope */
    } /* argc scope */

done:
    kva_free_pages(abuf, EXECVE_ARGBUF_PAGES);
    return ret;
}
