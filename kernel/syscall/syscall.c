#include "syscall.h"
#include "syscall_util.h"
#include "uaccess.h"
#include "sched.h"
#include "proc.h"
#include "vfs.h"
#include "initrd.h"
#include "vmm.h"
#include "pmm.h"
#include "kva.h"
#include "elf.h"
#include "printk.h"
#include "arch.h"
#include <stdint.h>
#include <stddef.h>

extern void isr_post_dispatch(void);

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
 * sys_writev — syscall 20
 *
 * arg1 = fd
 * arg2 = user pointer to struct iovec array
 * arg3 = iovcnt (number of vectors)
 *
 * musl's __stdio_write uses writev instead of write for buffered I/O.
 * We implement it by iterating over the iovec array and calling the fd's
 * write op for each non-empty vector.
 *
 * struct iovec { void *iov_base; size_t iov_len; }  — 16 bytes on x86-64.
 * Returns total bytes written on success, negative errno on failure.
 * Requires CAP_KIND_VFS_WRITE.
 */
typedef struct {
    uint64_t iov_base;   /* user pointer */
    uint64_t iov_len;
} aegis_iovec_t;

static uint64_t
sys_writev(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();

    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_WRITE, CAP_RIGHTS_WRITE) != 0) {
        return (uint64_t)-(int64_t)ENOCAP;
    }

    if (arg1 >= PROC_MAX_FDS || !proc->fds[arg1].ops ||
        !proc->fds[arg1].ops->write)
        return (uint64_t)-9;   /* EBADF */

    /* Reject unreasonable iovcnt before multiplying to avoid overflow. */
    if (arg3 > 1024)
        return (uint64_t)-(int64_t)22;  /* EINVAL */

    /* Validate the iovec array itself is in user space */
    if (!user_ptr_valid(arg2, arg3 * sizeof(aegis_iovec_t)))
        return (uint64_t)-14;  /* EFAULT */

    uint64_t total = 0;
    uint64_t i;
    for (i = 0; i < arg3; i++) {
        aegis_iovec_t iov;
        /* Copy the iovec descriptor from user space */
        copy_from_user(&iov,
                       (const void *)(uintptr_t)(arg2 + i * sizeof(aegis_iovec_t)),
                       sizeof(aegis_iovec_t));

        if (iov.iov_len == 0)
            continue;

        if (!user_ptr_valid(iov.iov_base, iov.iov_len))
            return (uint64_t)-14;  /* EFAULT */

        int r = proc->fds[arg1].ops->write(
                    proc->fds[arg1].priv,
                    (const void *)(uintptr_t)iov.iov_base,
                    iov.iov_len);
        if (r < 0)
            return (uint64_t)(int64_t)r;
        total += (uint64_t)r;
    }
    return total;
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
    if (sched_current()->is_user) {
        aegis_process_t *proc = (aegis_process_t *)sched_current();
        proc->exit_status = arg1 & 0xFF;
    }
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
    if (!user_ptr_valid(arg1, 1))
        return (uint64_t)-14;  /* EFAULT */
    /* Copy byte-by-byte until null terminator — never read past the string.
     * A bulk 256-byte copy can cross a page boundary if the path string is
     * placed near the end of the mapped user stack (e.g. argv[1] from execve
     * is placed within 256 bytes of USER_STACK_TOP), causing a kernel #PF.
     * Stopping at the null avoids reading into the unmapped page above the stack. */
    char kpath[256];
    {
        uint64_t i;
        for (i = 0; i < 255; i++) {
            if (!user_ptr_valid(arg1 + i, 1))
                return (uint64_t)-14;  /* EFAULT */
            char c;
            copy_from_user(&c, (const void *)(uintptr_t)(arg1 + i), 1);
            kpath[i] = c;
            if (c == '\0') break;
        }
        kpath[255] = '\0';
    }
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

    /* Single read call — return whatever the VFS gives us.
     * This matches Unix semantics: read() returns ≤ n bytes, callers loop.
     * Do NOT loop here: character devices (kbd) always return 1 byte and
     * never return 0, so a loop would block until arg3 bytes are read. */
    char kbuf[256];
    uint64_t n = arg3;
    if (n > sizeof(kbuf)) n = sizeof(kbuf);
    int got = f->ops->read(f->priv, kbuf, f->offset, n);
    if (got <= 0) return 0;
    copy_to_user((void *)(uintptr_t)arg2, kbuf, (uint64_t)got);
    f->offset += (uint64_t)got;
    return (uint64_t)got;
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

#define WNOHANG  1

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

    /* Guard: mmap_base must stay in user address space (below 0x800000000000).
     * Without this check, a large enough mapping would overflow mmap_base into
     * the kernel-half VA range, corrupting shared kernel page tables. */
    if (base + len > 0x0000800000000000ULL || base + len < base)
        return (uint64_t)-(int64_t)12;  /* -ENOMEM */
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

/* ERANGE — result too large; not defined in kernel headers, value matches Linux. */
#ifndef ERANGE
#define ERANGE 34
#endif

/* EBADF — bad file descriptor; value matches Linux. */
#ifndef EBADF
#define EBADF 9
#endif

/* ENOTDIR — not a directory; value matches Linux. */
#ifndef ENOTDIR
#define ENOTDIR 20
#endif

/*
 * sys_getdents64 — syscall 217
 *
 * arg1 = fd (must be a directory)
 * arg2 = user pointer to output buffer
 * arg3 = buffer size in bytes
 *
 * Fills the buffer with linux_dirent64 records for entries starting at the
 * current directory offset (f->offset), advancing the offset for each entry
 * consumed.  Returns total bytes written, or negative errno.
 *
 * The linux_dirent64 layout matches the Linux kernel ABI exactly:
 *   d_ino    uint64  inode number (we use offset+1 as a synthetic value)
 *   d_off    int64   offset to next record (we use offset+1)
 *   d_reclen uint16  length of this record including trailing padding
 *   d_type   uint8   DT_REG=8, DT_DIR=4
 *   d_name   char[]  null-terminated name, padded to 8-byte record boundary
 *
 * Record size formula: (19 + namelen + 1) rounded up to 8 bytes.
 *   19 = sizeof(d_ino)+sizeof(d_off)+sizeof(d_reclen)+sizeof(d_type)
 *        = 8 + 8 + 2 + 1 = 19
 */

/* linux_dirent64 matches the Linux kernel structure exactly. */
typedef struct {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[1];  /* flexible — actual size determined by d_reclen */
} __attribute__((packed)) linux_dirent64_t;

static uint64_t
sys_getdents64(uint64_t fd_num, uint64_t dirp, uint64_t count)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (fd_num >= PROC_MAX_FDS) return (uint64_t)-(int64_t)EBADF;
    vfs_file_t *f = &proc->fds[fd_num];
    if (!f->ops) return (uint64_t)-(int64_t)EBADF;
    if (!f->ops->readdir) return (uint64_t)-(int64_t)ENOTDIR;
    if (!user_ptr_valid(dirp, count)) return (uint64_t)-(int64_t)14; /* EFAULT */

    uint64_t written = 0;
    char name[256];
    uint8_t type;

    while (1) {
        if (f->ops->readdir(f->priv, f->offset, name, &type) != 0) break;

        /* Record size: fixed header (19 bytes) + name + null, rounded up to 8 */
        uint64_t namelen = 0;
        while (name[namelen]) namelen++;
        uint16_t reclen = (uint16_t)(19 + namelen + 1);
        reclen = (uint16_t)((reclen + 7) & ~7);

        if (written + reclen > count) break;

        /* Build dirent in kernel buffer, then copy_to_user */
        uint8_t kbuf[300];
        linux_dirent64_t *d = (linux_dirent64_t *)kbuf;
        d->d_ino    = f->offset + 1;
        d->d_off    = (int64_t)(f->offset + 1);
        d->d_reclen = reclen;
        d->d_type   = type;
        uint64_t i;
        for (i = 0; i <= namelen; i++) d->d_name[i] = name[i];
        /* zero-pad trailing bytes to reach record boundary */
        for (i = 1 + namelen; i < (uint64_t)(reclen - 19); i++) d->d_name[i] = '\0';

        copy_to_user((void *)(uintptr_t)(dirp + written), kbuf, reclen);
        written += reclen;
        f->offset++;
    }
    return written;
}

/*
 * sys_getcwd — syscall 79
 *
 * arg1 = user buffer pointer
 * arg2 = buffer size in bytes
 *
 * Copies proc->cwd (including null terminator) into the user buffer.
 * Returns the byte count (including null terminator) on success — Linux
 * sys_getcwd ABI (the libc wrapper returns the pointer, the raw syscall
 * returns the length).  -ERANGE if the buffer is too small, -EFAULT if
 * the pointer is invalid.
 */
static uint64_t
sys_getcwd(uint64_t buf_ptr, uint64_t size)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    uint64_t len = 0;
    while (proc->cwd[len]) len++;
    len++;  /* include null terminator */
    if (size < len) return (uint64_t)-(int64_t)ERANGE;
    if (!user_ptr_valid(buf_ptr, len)) return (uint64_t)-(int64_t)14; /* EFAULT */
    copy_to_user((void *)(uintptr_t)buf_ptr, proc->cwd, len);
    return len;  /* raw syscall returns byte count, not pointer */
}

/*
 * sys_chdir — syscall 80
 *
 * arg1 = user pointer to null-terminated path
 *
 * Sets proc->cwd to the provided path (up to 255 bytes + null).
 * Returns 0 on success, -EFAULT if the pointer is invalid.
 * Note: no filesystem validation in Phase 15; shell is responsible for
 * passing valid paths.
 */
static uint64_t
sys_chdir(uint64_t path_ptr)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (!user_ptr_valid(path_ptr, 1)) return (uint64_t)-(int64_t)14; /* EFAULT */
    uint64_t i;
    for (i = 0; i < 255; i++) {
        char c;
        copy_from_user(&c, (const void *)(uintptr_t)(path_ptr + i), 1);
        proc->cwd[i] = c;
        if (c == '\0') break;
    }
    proc->cwd[255] = '\0';
    return 0;
}

/*
 * sys_getppid — syscall 110
 *
 * Returns the parent PID of the calling process.
 * No capability gate — a process may always query its own parent.
 */
static uint64_t
sys_getppid(void)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    return (uint64_t)proc->ppid;
}

/* ── Stubs ─────────────────────────────────────────────────────────────────
 * musl startup calls these; they do not require real implementations in
 * Phase 14 with a single short-lived process.
 */
static uint64_t sys_exit_group(uint64_t arg1)
{
    if (sched_current()->is_user) {
        aegis_process_t *proc = (aegis_process_t *)sched_current();
        proc->exit_status = arg1 & 0xFF;
    }
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
static uint64_t
sys_fork(syscall_frame_t *frame)
{
    aegis_task_t    *parent_task = sched_current();
    aegis_process_t *parent      = (aegis_process_t *)parent_task;


    /* 1. Allocate child PCB */
    aegis_process_t *child = kva_alloc_pages(1);
    if (!child)
        return (uint64_t)-(int64_t)12;   /* -ENOMEM */

    /* 2. Copy parent fields */
    uint32_t ci;
    for (ci = 0; ci < PROC_MAX_FDS; ci++)
        child->fds[ci] = parent->fds[ci];
    for (ci = 0; ci < CAP_TABLE_SIZE; ci++)
        child->caps[ci] = parent->caps[ci];
    child->brk             = parent->brk;
    child->mmap_base       = parent->mmap_base;
    child->fs_base         = parent->fs_base;
    __builtin_memcpy(child->cwd, parent->cwd, sizeof(parent->cwd));
    child->pid             = proc_alloc_pid();
    child->ppid            = parent->pid;
    child->exit_status     = 0;
    child->task.state      = TASK_RUNNING;
    child->task.waiting_for = 0;
    child->task.is_user    = 1;
    child->task.tid        = child->pid;   /* use pid as tid */
    child->task.stack_pages = 1;

    /* 3. Create child PML4 */
    child->pml4_phys = vmm_create_user_pml4();

    /* 4. Copy parent user pages into child PML4 */
    if (vmm_copy_user_pages(parent->pml4_phys, child->pml4_phys) != 0) {
        kva_free_pages(child, 1);
        return (uint64_t)-(int64_t)12;           /* -ENOMEM */
    }

    /* 5. Allocate child kernel stack */
    uint8_t *kstack = kva_alloc_pages(1);
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
    uint64_t *sp = (uint64_t *)(kstack + 4096);

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
    child->task.kernel_stack_top = (uint64_t)(uintptr_t)(kstack + 4096);

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
static uint64_t
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

#define USER_STACK_TOP_EXEC   0x7FFFFFFF000ULL
#define USER_STACK_NPAGES     4ULL
#define USER_STACK_BASE_EXEC  (USER_STACK_TOP_EXEC - USER_STACK_NPAGES * 4096ULL)

/* execve_argbuf_t — argv working storage allocated from kva.
 *
 * argv_bufs[64][256] alone is 16 KB — larger than a child process's
 * 4-page kernel stack.  Allocating from kva avoids the overflow.
 * Size: 64*256 + 65*8 + 64*8 = 17416 bytes → 5 kva pages.
 */
typedef struct {
    char     argv_bufs[64][256];
    char    *argv_ptrs[65];
    uint64_t str_ptrs[64];
} execve_argbuf_t;

#define EXECVE_ARGBUF_PAGES 5   /* ceil(17416 / 4096) */

static uint64_t
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
            if (!phys) { ret = (uint64_t)-(int64_t)12; goto done; }  /* ENOMEM */
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

    /* 9. Redirect SYSRET to new ELF entry point */
    frame->rip      = er.entry;
    frame->user_rsp = sp_va;
    /* ret = 0 (success) */
        } /* argc2 scope */
    } /* argc scope */

done:
    kva_free_pages(abuf, EXECVE_ARGBUF_PAGES);
    return ret;
}

uint64_t
syscall_dispatch(syscall_frame_t *frame, uint64_t num,
                 uint64_t arg1, uint64_t arg2, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6)
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
    case 20: return sys_writev(arg1, arg2, arg3);
    case 39: return sys_getpid();
    case 57: return sys_fork(frame);
    case 59: return sys_execve(frame, arg1, arg2, arg3);
    case 60: return sys_exit(arg1);
    case 61: return sys_waitpid(arg1, arg2, arg3);
    case  79: return sys_getcwd(arg1, arg2);
    case  80: return sys_chdir(arg1);
    case 217: return sys_getdents64(arg1, arg2, arg3);
    case 110: return sys_getppid();
    case 158: return sys_arch_prctl(arg1, arg2);
    case 218: return sys_set_tid_address(arg1);
    case 231: return sys_exit_group(arg1);
    case 273: return sys_set_robust_list(arg1, arg2);
    default: return (uint64_t)-1;   /* ENOSYS */
    }
}
