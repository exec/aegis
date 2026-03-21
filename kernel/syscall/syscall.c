#include "syscall.h"
#include "syscall_util.h"
#include "uaccess.h"
#include "sched.h"
#include "proc.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "printk.h"
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

uint64_t
syscall_dispatch(uint64_t num, uint64_t arg1,
                 uint64_t arg2, uint64_t arg3)
{
    switch (num) {
    case 0:  return sys_read(arg1, arg2, arg3);
    case 1:  return sys_write(arg1, arg2, arg3);
    case 2:  return sys_open(arg1, arg2, arg3);
    case 3:  return sys_close(arg1);
    case 12: return sys_brk(arg1);
    case 60: return sys_exit(arg1);
    default: return (uint64_t)-1;   /* ENOSYS */
    }
}
