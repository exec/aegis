#include "proc.h"
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "kva.h"
#include "printk.h"
#include "arch.h"
#include "console.h"
#include "kbd_vfs.h"
#include "kbd.h"
#include "vma.h"
#include "spinlock.h"
#include "random.h"
#include "initrd.h"
#include "../drivers/fb.h"
#include "vfs.h"
#include <stdint.h>
#include <stddef.h>

static uint32_t s_next_pid = 1;
static spinlock_t pid_lock = SPINLOCK_INIT;

uint32_t
proc_alloc_pid(void)
{
    irqflags_t fl = spin_lock_irqsave(&pid_lock);
    uint32_t pid = s_next_pid++;
    spin_unlock_irqrestore(&pid_lock, fl);
    return pid;
}

_Static_assert(offsetof(aegis_process_t, task) == 0,
               "aegis_process_t: task must be at offset 0 for safe cast");

/*
 */
extern const unsigned char _binary_init_bin_start[];
extern const unsigned char _binary_init_bin_end[];


/* 16KB kernel stack for the user process (4 pages, matching sched task stacks).
 * The PIT ISR fires while the process is in user mode and uses the kernel stack
 * (via TSS RSP0). TCP RX processing through virtio-net -> ip_rx -> tcp_rx needs
 * more than 4KB of stack depth; 4 pages avoids double-fault on deep call chains. */
#define STACK_PAGES  4UL
#define STACK_SIZE  (STACK_PAGES * 4096UL)

/*
 * User stack layout:
 *   top  = USER_STACK_TOP  (= 0x7FFFFFFF000, 16-byte aligned)
 *   base = [USER_STACK_BASE, USER_STACK_TOP) — USER_STACK_NPAGES pages
 * 0x7FFFFFFF000 ends in 0x000 — 4096-byte aligned → 16-byte aligned. ✓
 * Per AMD64 ABI, RSP on entry to _start must be 16-byte aligned.
 */
#define USER_STACK_TOP     0x7FFFFFFF000ULL
#define USER_STACK_NPAGES  4ULL
#define USER_STACK_BASE    (USER_STACK_TOP - USER_STACK_NPAGES * 4096ULL)

/* Arch-specific user-mode entry trampoline.
 * x86-64: bare iretq label in syscall_entry.asm.
 * ARM64: ERET trampoline in proc_enter.S.
 * Used as a return-address slot in the initial kernel stack frame. */
extern void proc_enter_user(void);

void
proc_spawn(const uint8_t *elf_data, size_t elf_len)
{
    /* Allocate process control block (one kva page — higher-half VA). */
    aegis_process_t *proc = kva_alloc_pages(1);

    /* Allocate kernel stack (STACK_PAGES kva pages — per-process higher-half VA).
     * kva pages are mapped into pd_hi (shared with user PML4s), so this
     * stack VA is reachable regardless of which PML4 is loaded in CR3.
     * Each proc_spawn call gets a distinct kva-mapped VA.
     * 4 pages (16KB) matches the sched task stack size and is sufficient to
     * handle deep PIT ISR → virtio → tcp_rx call chains from user mode. */
    uint8_t *kstack = kva_alloc_pages(STACK_PAGES);

    /* Create per-process page tables (kernel high entries shared) */
    proc->pml4_phys = vmm_create_user_pml4();

    /* Load ELF into the user address space */
    elf_load_result_t er;
    elf_load_result_t interp_er;
    int has_interp = 0;
    __builtin_memset(&interp_er, 0, sizeof(interp_er));

    if (elf_load(proc->pml4_phys, elf_data, elf_len, 0, &er) != 0) {
        printk("[PROC] FAIL: ELF parse error\n");
        panic_halt("[PROC] FAIL: ELF parse error");
    }

    /* If the ELF has a PT_INTERP (dynamically linked), load the interpreter
     * at INTERP_BASE. Without this, _start jumps through an unresolved PLT
     * stub to address 0x0 — the root cause of the q35-without-NVMe crash. */
    has_interp = (er.interp[0] != '\0');
    if (has_interp) {
        const uint8_t *interp_data;
        uint64_t interp_size;
        void    *interp_buf   = (void *)0;
        uint64_t interp_pages = 0;

        vfs_file_t interp_f;
        if (initrd_open(er.interp, &interp_f) == 0) {
            interp_data = (const uint8_t *)initrd_get_data(&interp_f);
            interp_size = (uint64_t)initrd_get_size(&interp_f);
        } else {
            vfs_file_t vf;
            int vr = vfs_open(er.interp, 0, &vf);
            if (vr != 0) {
                printk("[PROC] FAIL: interpreter not found: %s\n", er.interp);
                panic_halt("[PROC] FAIL: interpreter not found: ...");
            }
            interp_pages = (vf.size + 4095ULL) / 4096ULL;
            interp_buf = kva_alloc_pages(interp_pages);
            if (!interp_buf) {
                printk("[PROC] FAIL: OOM loading interpreter\n");
                panic_halt("[PROC] FAIL: OOM loading interpreter");
            }
            int rr = vf.ops->read(vf.priv, interp_buf, 0, vf.size);
            if (rr < 0) {
                printk("[PROC] FAIL: error reading interpreter\n");
                panic_halt("[PROC] FAIL: error reading interpreter");
            }
            interp_data = (const uint8_t *)interp_buf;
            interp_size = vf.size;
        }

        if (elf_load(proc->pml4_phys, interp_data, (size_t)interp_size,
                     INTERP_BASE, &interp_er) != 0) {
            printk("[PROC] FAIL: interpreter ELF parse error\n");
            panic_halt("[PROC] FAIL: interpreter ELF parse error");
        }
        if (interp_buf)
            kva_free_pages(interp_buf, interp_pages);
    }

    uint64_t entry_rip = has_interp ? interp_er.entry : er.entry;
    uint64_t brk_start = er.brk;

    /* Allocate and map user stack page.
     * vmm_zero_page ensures the initial stack contents are zero.
     * We then write a minimal argv so that "init" is argv[0]:
     *   [RSP+0]  = argc = 1
     *   [RSP+8]  = argv[0] pointer → string near top of stack
     *   [RSP+16] = 0 (argv NULL terminator)
     *   [RSP+24] = 0 (envp NULL)
     *   [RSP+32] = 0 (AT_NULL)
     * The string ""init"\0" is placed at USER_STACK_TOP - 16 (within page). */
    {
        uint64_t pn;
        for (pn = 0; pn < USER_STACK_NPAGES; pn++) {
            uint64_t user_stack_phys = pmm_alloc_page();
            if (!user_stack_phys) {
                printk("[PROC] FAIL: OOM allocating user stack\n");
                panic_halt("[PROC] FAIL: OOM allocating user stack");
            }
            vmm_zero_page(user_stack_phys);
            vmm_map_user_page(proc->pml4_phys,
                              USER_STACK_BASE + pn * 4096ULL, user_stack_phys,
                              VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE);
        }
    }

    /* Build SysV ABI initial stack.
     * Layout (top → bottom): argv string, AT_RANDOM data, pointer table.
     * Matches the layout used by sys_execve. */
    uint64_t rsp_init;
    {
        uint64_t sp_va = USER_STACK_TOP;

        /* Write argv[0] string "- init" (login shell prefix) at top of stack */
        char login_name[16];
        login_name[0] = '-';
        __builtin_memcpy(login_name + 1, "init", 5); /* includes '\0' */
        uint64_t slen = __builtin_strlen(login_name) + 1;
        sp_va -= slen;
        vmm_write_user_bytes(proc->pml4_phys, sp_va, login_name, slen);
        uint64_t str_va = sp_va;

        /* Write 16 random bytes for AT_RANDOM */
        sp_va -= 16;
        {
            uint8_t rand_bytes[16];
            random_get_bytes(rand_bytes, 16);
            vmm_write_user_bytes(proc->pml4_phys, sp_va, rand_bytes, 16);
        }
        uint64_t at_random_va = sp_va;

        /* Align to 8 bytes for pointer table */
        sp_va &= ~7ULL;

        /* Pointer table: argc + argv[0] + NULL + envp NULL + auxv pairs + AT_NULL.
         * Base: 4 + 10 + 2 = 16 qwords.  With interp: +4 = 20 qwords. */
        uint64_t auxv_qwords = has_interp ? 16 : 12;
        uint64_t table_qwords = 4 + auxv_qwords; /* argc,argv[0],NULL,envpNULL + auxv */
        uint64_t table_bytes  = table_qwords * 8;
        sp_va -= table_bytes;
        /* Ensure RSP % 16 == 8 at _start per SysV ABI */
        if ((sp_va % 16) != 8) sp_va -= 8;

        uint64_t wp = sp_va;
        vmm_write_user_u64(proc->pml4_phys, wp, 1ULL);        wp += 8; /* argc = 1 */
        vmm_write_user_u64(proc->pml4_phys, wp, str_va);      wp += 8; /* argv[0] */
        vmm_write_user_u64(proc->pml4_phys, wp, 0ULL);        wp += 8; /* argv NULL */
        vmm_write_user_u64(proc->pml4_phys, wp, 0ULL);        wp += 8; /* envp NULL */
        /* AT_PHDR (3) */
        vmm_write_user_u64(proc->pml4_phys, wp, 3ULL);        wp += 8;
        vmm_write_user_u64(proc->pml4_phys, wp, er.phdr_va);  wp += 8;
        /* AT_PHNUM (5) */
        vmm_write_user_u64(proc->pml4_phys, wp, 5ULL);        wp += 8;
        vmm_write_user_u64(proc->pml4_phys, wp, (uint64_t)er.phdr_count); wp += 8;
        /* AT_PAGESZ (6) */
        vmm_write_user_u64(proc->pml4_phys, wp, 6ULL);        wp += 8;
        vmm_write_user_u64(proc->pml4_phys, wp, 4096ULL);     wp += 8;
        /* AT_ENTRY (9) */
        vmm_write_user_u64(proc->pml4_phys, wp, 9ULL);        wp += 8;
        vmm_write_user_u64(proc->pml4_phys, wp, er.entry);    wp += 8;
        /* AT_RANDOM (25) */
        vmm_write_user_u64(proc->pml4_phys, wp, 25ULL);       wp += 8;
        vmm_write_user_u64(proc->pml4_phys, wp, at_random_va); wp += 8;
        if (has_interp) {
            /* AT_BASE (7) — interpreter load address */
            vmm_write_user_u64(proc->pml4_phys, wp, 7ULL);    wp += 8;
            vmm_write_user_u64(proc->pml4_phys, wp, INTERP_BASE); wp += 8;
            /* AT_PHENT (4) — program header entry size */
            vmm_write_user_u64(proc->pml4_phys, wp, 4ULL);    wp += 8;
            vmm_write_user_u64(proc->pml4_phys, wp, 56ULL);   wp += 8;
        }
        /* AT_NULL */
        vmm_write_user_u64(proc->pml4_phys, wp, 0ULL);        wp += 8;
        vmm_write_user_u64(proc->pml4_phys, wp, 0ULL);        wp += 8;

        /* rsp_va used for the iretq frame below */
        rsp_init = sp_va;
    }

    /*
     * Build initial kernel stack.
     *
     * ctx_switch pops: r15, r14, r13, r12, rbp, rbx, then ret.
     * The ret target is proc_enter_user (a bare iretq label).
     * iretq pops a ring-3 frame: RIP, CS, RFLAGS, RSP, SS.
     *
     * Stack layout from low (RSP) to high:
     *   [r15=0][r14=0][r13=0][r12=0][rbp=0][rbx=0]
     *   [proc_enter_user]
     *   [entry_rip][CS=ARCH_USER_CS][RFLAGS=0x202][user_RSP][SS=ARCH_USER_DS]
     *
     * Push order (high-to-low, decrementing sp):
     *   SS first (highest address), r15 last (lowest = task.sp value).
     */
    uint64_t *sp = (uint64_t *)(kstack + STACK_SIZE);

#ifdef __aarch64__
    /* ARM64: build a frame for proc_enter_user_arm64 trampoline.
     * The trampoline loads TTBR0, sets SP_EL0, ELR_EL1, SPSR, and ERETs.
     *
     * Stack layout (high → low):
     *   [user_pml4_phys]  — trampoline loads into TTBR0
     *   [user_sp]         — trampoline writes to SP_EL0
     *   [entry_rip]       — trampoline writes to ELR_EL1
     *   [spsr = 0]        — EL0, interrupts enabled (DAIF clear)
     * Then ctx_switch callee-saves (12 slots):
     *   [x29=0][x30=proc_enter_user] ... [x19=0][x20=0]
     */
    *--sp = proc->pml4_phys;
    *--sp = rsp_init;               /* user SP */
    *--sp = entry_rip;              /* ELR (entry point) */
    *--sp = 0;                      /* SPSR: EL0, all interrupts enabled */

    /* ctx_switch callee-save frame (matches ctx_switch.S pop order) */
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
    *--sp = (uint64_t)(uintptr_t)proc_enter_user; /* x30 (lr) → trampoline */
    *--sp = 0;                          /* x29 (fp) */
#else
    /* x86-64: iretq frame + ctx_switch callee-saves. */
    *--sp = (uint64_t)ARCH_USER_DS; /* SS  — user data | RPL=3    */
    *--sp = rsp_init;             /* RSP */
    *--sp = 0x202ULL;           /* RFLAGS — IF=1, reserved bit 1  */
    *--sp = (uint64_t)ARCH_USER_CS; /* CS  — user code | RPL=3     */
    *--sp = entry_rip;          /* RIP — ELF entry point          */
    *--sp = proc->pml4_phys;    /* user PML4 phys — popped by proc_enter_user */
    *--sp = (uint64_t)(uintptr_t)proc_enter_user; /* ret → CR3 switch + iretq */
    *--sp = 0;                  /* rbx */
    *--sp = 0;                  /* rbp */
    *--sp = 0;                  /* r12 */
    *--sp = 0;                  /* r13 */
    *--sp = 0;                  /* r14 */
    *--sp = 0;                  /* r15 ← task.sp */
#endif

    /* Initialize task fields */
    proc->task.sp               = (uint64_t)(uintptr_t)sp;
    proc->task.stack_base       = kstack;
    proc->task.kernel_stack_top = (uint64_t)(uintptr_t)(kstack + STACK_SIZE);
    proc->task.tid              = 0xFF;   /* placeholder TID for init process */
    proc->task.is_user          = 1;
    proc->task.stack_pages      = (uint32_t)STACK_PAGES;

    /* Allocate refcounted fd table — all slots start as free (ops == NULL). */
    proc->fd_table = fd_table_alloc();
    if (!proc->fd_table) {
        printk("[PROC] FAIL: OOM allocating fd_table\n");
        panic_halt("[PROC] FAIL: OOM allocating fd_table");
    }

    /* Zero cap table — all slots start empty.
     * kva_alloc_pages maps raw physical frames without zeroing; the loop is
     * required to ensure all slots start as CAP_KIND_NULL (= 0).
     * fd_table_alloc() handles zeroing the fd table separately. */
    {
        uint32_t ci;
        for (ci = 0; ci < CAP_TABLE_SIZE; ci++) {
            proc->caps[ci].kind   = CAP_KIND_NULL;
            proc->caps[ci].rights = 0;
        }
    }

    /* Zero exec_caps — pre-registered caps that will be applied by execve. */
    {
        uint32_t ci;
        for (ci = 0; ci < CAP_TABLE_SIZE; ci++) {
            proc->exec_caps[ci].kind   = CAP_KIND_NULL;
            proc->exec_caps[ci].rights = 0;
        }
    }

    proc->pid              = proc_alloc_pid();   /* 1 for init */
    proc->tgid             = proc->pid;
    proc->thread_count     = 1;
    proc->ppid             = 0;
    proc->uid              = 0;
    proc->gid              = 0;
    proc->pgid             = proc->pid;
    proc->sid              = proc->pid;  /* init is its own session leader */
    proc->umask            = 022U;
    proc->stop_signum      = 0;
    proc->cwd[0]           = '/';
    proc->cwd[1]           = '\0';
    proc->exit_status      = 0;
    proc->mmap_free_count  = 0;

    /* Initialize VMA tracking */
    vma_init((struct aegis_process *)proc);
    /* Record user stack VMA */
    vma_insert((struct aegis_process *)proc, USER_STACK_BASE, USER_STACK_NPAGES * 4096ULL,
               0x01 | 0x02,  /* PROT_READ | PROT_WRITE */
               VMA_STACK);
    /* exe_path: set to /bin/init for the init process */
    __builtin_memcpy(proc->exe_path, "/bin/init", 10);

    proc->pending_signals  = 0;
    proc->signal_mask      = 0;
    __builtin_memset(proc->sigactions, 0, sizeof(proc->sigactions));
    proc->task.state       = TASK_RUNNING;
    proc->task.waiting_for = 0;

    /* Grant initial capabilities to this user process.
     * cap_grant returns the slot index (>= 0) on success or -ENOCAP if the
     * table is full. With CAP_TABLE_SIZE = 16 and 8 grants, this cannot fail. */

    /* Grant open capability. */
    if (cap_grant(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_OPEN, CAP_RIGHTS_READ) < 0) {
        printk("[CAP] FAIL: cap_grant VFS_OPEN returned -ENOCAP\n");
        panic_halt("[CAP] FAIL: cap_grant VFS_OPEN returned -ENOCAP");
    }

    /* Grant write capability. */
    if (cap_grant(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_WRITE, CAP_RIGHTS_WRITE) < 0) {
        printk("[CAP] FAIL: cap_grant VFS_WRITE returned -ENOCAP\n");
        panic_halt("[CAP] FAIL: cap_grant VFS_WRITE returned -ENOCAP");
    }

    /* Grant read capability. */
    if (cap_grant(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_READ, CAP_RIGHTS_READ) < 0) {
        printk("[CAP] FAIL: cap_grant VFS_READ returned -ENOCAP\n");
        panic_halt("[CAP] FAIL: cap_grant VFS_READ returned -ENOCAP");
    }

    /* Grant auth capability — login needs CAP_KIND_AUTH to open /etc/shadow. */
    if (cap_grant(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_AUTH, CAP_RIGHTS_READ) < 0) {
        printk("[CAP] FAIL: cap_grant AUTH returned -ENOCAP\n");
        panic_halt("[CAP] FAIL: cap_grant AUTH returned -ENOCAP");
    }

    /* Grant cap-delegation capability (reserved for future use). */
    if (cap_grant(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_CAP_GRANT, CAP_RIGHTS_READ) < 0) {
        printk("[CAP] FAIL: cap_grant CAP_GRANT returned -ENOCAP\n");
        panic_halt("[CAP] FAIL: cap_grant CAP_GRANT returned -ENOCAP");
    }

    /* Grant setuid capability — login calls sys_setuid/setgid after auth. */
    if (cap_grant(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_SETUID, CAP_RIGHTS_WRITE) < 0) {
        printk("[CAP] FAIL: cap_grant SETUID returned -ENOCAP\n");
        panic_halt("[CAP] FAIL: cap_grant SETUID returned -ENOCAP");
    }

    /* Grant network socket capability — required for sys_socket. */
    if (cap_grant(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_NET_SOCKET, CAP_RIGHTS_READ) < 0) {
        printk("[CAP] FAIL: cap_grant NET_SOCKET returned -ENOCAP\n");
        panic_halt("[CAP] FAIL: cap_grant NET_SOCKET returned -ENOCAP");
    }

    /* Grant network admin capability — required for sys_netcfg (DHCP daemon). */
    if (cap_grant(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_NET_ADMIN, CAP_RIGHTS_WRITE) < 0) {
        printk("[CAP] FAIL: cap_grant NET_ADMIN returned -ENOCAP\n");
        panic_halt("[CAP] FAIL: cap_grant NET_ADMIN returned -ENOCAP");
    }

    /* Grant proc_read capability — reading other processes' /proc. */
    if (cap_grant(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_PROC_READ, CAP_RIGHTS_READ) < 0) {
        printk("[CAP] FAIL: cap_grant PROC_READ returned -ENOCAP\n");
        panic_halt("[CAP] FAIL: cap_grant PROC_READ returned -ENOCAP");
    }

    /* Pre-open fd 1 (stdout) to the console device.
     * User process inherits stdout without a sys_open call. */
    proc->fd_table->fds[1] = *console_open();

    /* Pre-open fd 0 (stdin) to keyboard device. */
    proc->fd_table->fds[0] = *kbd_vfs_open();

    /* Pre-open fd 2 (stderr) to console device. */
    proc->fd_table->fds[2] = *console_open();

    /* Initialise heap break to top of ELF segments. */
    proc->brk = brk_start;

    /* Initialise mmap bump allocator base (112 TB — safely above heap, below stack). */
    proc->mmap_base = 0x0000700000000000ULL;

    /* FS base starts at zero; arch_prctl(ARCH_SET_FS) sets it at musl startup. */
    proc->task.fs_base = 0;

    /* Register init as the terminal foreground process group so that
     * TIOCGPGRP returns proc->pgid immediately on first access.
     * Without this, oksh's job-control startup loop sees pgid=0 != its own
     * pgid and sends itself SIGTTIN repeatedly. */
    kbd_set_tty_pgrp(proc->pgid);

    printk("[CAP] OK: 9 capabilities granted to init\n");

    sched_add(&proc->task);
}

void
proc_spawn_init(void)
{
    proc_spawn((const uint8_t *)_binary_init_bin_start, (size_t)(_binary_init_bin_end - _binary_init_bin_start));
}

/* arch_get_current_pml4 — return current user process PML4 phys addr.
 * Used by ARM64 fork_child_return to load TTBR0. */
uint64_t arch_get_current_pml4(void) {
    aegis_task_t *t = sched_current();
    if (t && t->is_user)
        return ((aegis_process_t *)t)->pml4_phys;
    return 0;
}

uint64_t arch_get_current_fs_base(void) {
    aegis_task_t *t = sched_current();
    if (t)
        return t->fs_base;
    return 0;
}

void arch_save_current_fs_base(uint64_t val) {
    aegis_task_t *t = sched_current();
    if (t)
        t->fs_base = val;
}

aegis_process_t *
proc_find_by_pid(uint32_t pid)
{
    aegis_task_t *cur = sched_current();
    if (!cur)
        return (aegis_process_t *)0;

    /* Check current task first */
    if (cur->is_user) {
        aegis_process_t *p = (aegis_process_t *)cur;
        if (p->pid == pid)
            return p;
    }

    /* Walk the circular list */
    aegis_task_t *t = cur->next;
    while (t != cur) {
        if (t->is_user) {
            aegis_process_t *p = (aegis_process_t *)t;
            if (p->pid == pid)
                return p;
        }
        t = t->next;
    }
    return (aegis_process_t *)0;
}
