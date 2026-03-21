#include "syscall.h"
#include "syscall_util.h"
#include "uaccess.h"
#include "sched.h"
#include "proc.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "printk.h"
#include "arch.h"
#include <stdint.h>

/*
 * sys_write — syscall 1
 *
 * arg1 = fd
 * arg2 = user virtual address of buffer
 * arg3 = byte count
 *
 * Returns bytes written on success, negative errno on failure.
 * Requires CAP_KIND_VFS_WRITE capability. Routes through fd table.
 */
static uint64_t
sys_write(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();

    /* Capability gate — must hold VFS_WRITE before touching any fd. */
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_WRITE, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    if (arg1 >= PROC_MAX_FDS || !proc->fds[arg1].ops ||
        !proc->fds[arg1].ops->write)
        return (uint64_t)-9;   /* EBADF */

    if (!user_ptr_valid(arg2, arg3))
        return (uint64_t)-14;  /* EFAULT */

    int r = proc->fds[arg1].ops->write(
                proc->fds[arg1].priv,
                (const void *)(uintptr_t)arg2,
                arg3);
    return (uint64_t)(int64_t)r;
}

/*
 * sys_exit — syscall 60
 *
 * arg1 = exit code (ignored for Phase 5)
 * Calls sched_exit() which never returns.
 */
static uint64_t
sys_exit(uint64_t arg1)
{
    (void)arg1;
    sched_exit();
    __builtin_unreachable();
}

/*
 * sys_open — syscall 2
 *
 * arg1 = user pointer to null-terminated path string
 * arg2 = flags (ignored in Phase 10)
 * arg3 = mode (ignored in Phase 10)
 *
 * Returns fd on success, negative errno on failure.
 */
static uint64_t
sys_open(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_OPEN, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    (void)arg2; (void)arg3;   /* flags and mode ignored in Phase 10 */
    if (!user_ptr_valid(arg1, 256))
        return (uint64_t)-14;  /* EFAULT */
    char kpath[256];
    copy_from_user(kpath, (const void *)(uintptr_t)arg1, 256);
    kpath[255] = '\0';
    uint64_t fd;
    for (fd = 0; fd < PROC_MAX_FDS; fd++)
        if (!proc->fds[fd].ops) break;
    if (fd == PROC_MAX_FDS)
        return (uint64_t)-24;  /* EMFILE */

    int r = vfs_open(kpath, &proc->fds[fd]);
    if (r < 0)
        return (uint64_t)(int64_t)r;
    return fd;
}

/*
 * sys_read — syscall 0
 *
 * arg1 = fd
 * arg2 = user pointer to buffer
 * arg3 = byte count
 *
 * Returns bytes read (0 = EOF), negative errno on failure.
 */
static uint64_t
sys_read(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_READ, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    if (arg1 >= PROC_MAX_FDS)
        return (uint64_t)-9;   /* EBADF */
    vfs_file_t *f = &proc->fds[arg1];
    if (!f->ops)
        return (uint64_t)-9;   /* EBADF */
    if (!user_ptr_valid(arg2, arg3))
        return (uint64_t)-14;  /* EFAULT */

    char kbuf[256];
    uint64_t total = 0;
    while (total < arg3) {
        uint64_t n = arg3 - total;
        if (n > sizeof(kbuf)) n = sizeof(kbuf);
        int got = f->ops->read(f->priv, kbuf, f->offset, n);
        if (got <= 0) break;
        copy_to_user((void *)(uintptr_t)(arg2 + total), kbuf, (uint64_t)got);
        f->offset += (uint64_t)got;
        total     += (uint64_t)got;
    }
    return total;
}

/*
 * sys_close — syscall 3
 *
 * arg1 = fd
 *
 * Returns 0 on success, -9 (EBADF) if fd is invalid or already closed.
 */
static uint64_t
sys_close(uint64_t arg1)
{
    if (arg1 >= PROC_MAX_FDS)
        return (uint64_t)-9;   /* EBADF */
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    vfs_file_t *f = &proc->fds[arg1];
    if (!f->ops)
        return (uint64_t)-9;   /* EBADF */
    f->ops->close(f->priv);
    f->ops = (const vfs_ops_t *)0;
    return 0;
}

/*
 * sys_brk — syscall 12
 *
 * arg1 = requested new break address (0 = query current brk)
 *
 * Returns the new (or current) break address.
 * On OOM, returns the current break unchanged (Linux-compatible).
 * No capability gate — process expands its own address space only.
 *
 * arg1 is page-aligned upward before processing. proc->brk is always
 * page-aligned. musl's malloc passes exact byte offsets and expects the
 * kernel to return the actual (rounded-up) new break — this is correct.
 */
static uint64_t
sys_brk(uint64_t arg1)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();

    if (arg1 == 0)
        return proc->brk;  /* query */

    /* Clamp to user address space */
    if (arg1 >= 0x00007FFFFFFFFFFFULL)
        return proc->brk;

    /* Page-align upward so proc->brk is always page-aligned */
    arg1 = (arg1 + 4095UL) & ~4095UL;

    if (arg1 > proc->brk) {
        /* Grow: map pages [proc->brk, arg1) into this process's PML4 */
        uint64_t va;
        for (va = proc->brk; va < arg1; va += 4096UL) {
            uint64_t phys = pmm_alloc_page();
            if (!phys)
                return proc->brk;  /* OOM — return current brk unchanged */
            vmm_map_user_page(proc->pml4_phys, va, phys,
                              VMM_FLAG_PRESENT | VMM_FLAG_USER |
                              VMM_FLAG_WRITABLE);
        }
        proc->brk = arg1;
    } else if (arg1 < proc->brk) {
        /* Shrink: unmap and free pages [arg1, proc->brk) */
        uint64_t va;
        for (va = arg1; va < proc->brk; va += 4096UL) {
            uint64_t phys = vmm_phys_of_user(proc->pml4_phys, va);
            if (phys) {
                vmm_unmap_user_page(proc->pml4_phys, va);
                pmm_free_page(phys);
            }
        }
        proc->brk = arg1;
    }

    return proc->brk;
}

/*
 * sys_mmap — syscall 9
 *
 * Supports MAP_ANONYMOUS | MAP_PRIVATE only.
 * addr must be 0 (kernel chooses VA from bump allocator at 0x0000700000000000).
 * prot must be subset of PROT_READ | PROT_WRITE.
 * fd must be -1 (arg5), offset ignored (arg6).
 *
 * Each allocated page is zeroed before mapping — MAP_ANONYMOUS guarantee.
 * OOM rollback: already-mapped pages are unmapped and freed before returning -ENOMEM.
 * No capability gate — process expands its own address space only.
 */
#ifndef MAP_SHARED
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_ANONYMOUS   0x20
#define PROT_READ       0x01
#define PROT_WRITE      0x02
#endif

static uint64_t
sys_mmap(uint64_t arg1, uint64_t arg2, uint64_t arg3,
         uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    (void)arg6;  /* offset — not validated; musl always passes 0 for MAP_ANONYMOUS */

    aegis_process_t *proc = (aegis_process_t *)sched_current();

    /* Only support anonymous private mappings with addr==0. */
    if (arg1 != 0)
        return (uint64_t)-(int64_t)22;   /* EINVAL — fixed addr not supported */
    if (!(arg4 & MAP_ANONYMOUS))
        return (uint64_t)-(int64_t)22;   /* EINVAL — file-backed not supported */
    if (arg4 & MAP_SHARED)
        return (uint64_t)-(int64_t)22;   /* EINVAL — shared not supported */
    if ((arg3 & ~(uint64_t)(PROT_READ | PROT_WRITE)) != 0)
        return (uint64_t)-(int64_t)22;   /* EINVAL — exec/none prot not supported */
    if ((int64_t)arg5 != -1)
        return (uint64_t)-(int64_t)9;    /* EBADF */

    uint64_t len = (arg2 + 4095UL) & ~4095UL;  /* round up to page boundary */
    if (len == 0)
        return (uint64_t)-(int64_t)22;   /* EINVAL */

    uint64_t base = proc->mmap_base;
    uint64_t va;
    for (va = base; va < base + len; va += 4096UL) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            /* OOM: unmap already-mapped pages and return -ENOMEM.
             * MAP_FAILED = (void *)-1 — musl's allocator checks for this. */
            uint64_t v2;
            for (v2 = base; v2 < va; v2 += 4096UL) {
                uint64_t p = vmm_phys_of_user(proc->pml4_phys, v2);
                if (p) {
                    vmm_unmap_user_page(proc->pml4_phys, v2);
                    pmm_free_page(p);
                }
            }
            return (uint64_t)-(int64_t)12;  /* -ENOMEM */
        }
        /* MAP_ANONYMOUS guarantee: zero the page before mapping it.
         * musl's heap allocator reads free-list metadata from fresh pages;
         * stale PMM data would corrupt the allocator. */
        vmm_zero_page(phys);
        vmm_map_user_page(proc->pml4_phys, va, phys,
                          VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE);
    }

    proc->mmap_base += len;
    return base;
}

/*
 * sys_munmap — syscall 11
 *
 * arg1 = addr (must be page-aligned)
 * arg2 = length
 *
 * Frees physical pages for [addr, addr+len). Does not reclaim VA
 * (bump allocator — VA is not reused in Phase 14).
 * Returns 0 on success, -EINVAL if addr is not page-aligned.
 */
static uint64_t
sys_munmap(uint64_t arg1, uint64_t arg2)
{
    if (arg1 & 0xFFFUL)
        return (uint64_t)-(int64_t)22;   /* EINVAL — not page-aligned */

    aegis_process_t *proc = (aegis_process_t *)sched_current();
    uint64_t len = (arg2 + 4095UL) & ~4095UL;
    uint64_t va;
    for (va = arg1; va < arg1 + len; va += 4096UL) {
        uint64_t phys = vmm_phys_of_user(proc->pml4_phys, va);
        if (phys) {
            vmm_unmap_user_page(proc->pml4_phys, va);
            pmm_free_page(phys);
        }
    }
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
#ifndef ARCH_SET_FS
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#endif

static uint64_t
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

/* ── Stubs ─────────────────────────────────────────────────────────────────
 * musl startup calls these; they do not require real implementations in
 * Phase 14 with a single short-lived process.
 */
static uint64_t sys_exit_group(uint64_t arg1)
{
    (void)arg1;
    sched_exit();
    __builtin_unreachable();
}

static uint64_t sys_getpid(void) { return 1; }

static uint64_t
sys_rt_sigprocmask(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    (void)a; (void)b; (void)c; (void)d;
    return 0;
}

static uint64_t sys_set_tid_address(uint64_t arg1) { (void)arg1; return 1; }

static uint64_t sys_set_robust_list(uint64_t a, uint64_t b)
{
    (void)a; (void)b;
    return 0;
}

uint64_t
syscall_dispatch(uint64_t num, uint64_t arg1, uint64_t arg2,
                 uint64_t arg3, uint64_t arg4, uint64_t arg5,
                 uint64_t arg6)
{
    switch (num) {
    case  0: return sys_read(arg1, arg2, arg3);
    case  1: return sys_write(arg1, arg2, arg3);
    case  2: return sys_open(arg1, arg2, arg3);
    case  3: return sys_close(arg1);
    case  9: return sys_mmap(arg1, arg2, arg3, arg4, arg5, arg6);
    case 11: return sys_munmap(arg1, arg2);
    case 12: return sys_brk(arg1);
    case 14: return sys_rt_sigprocmask(arg1, arg2, arg3, arg4);
    case 39: return sys_getpid();
    case 60: return sys_exit(arg1);
    case 158: return sys_arch_prctl(arg1, arg2);
    case 218: return sys_set_tid_address(arg1);
    case 231: return sys_exit_group(arg1);
    case 273: return sys_set_robust_list(arg1, arg2);
    default: return (uint64_t)-1;   /* ENOSYS */
    }
}
