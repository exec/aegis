/* sys_io.c — I/O syscall implementations: read, write, writev, close */
#include "sys_impl.h"

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
uint64_t
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

    /* Retry loop: the write op (e.g. console_write_fn) may return a partial
     * count when the user buffer straddles a page boundary.  Loop until all
     * bytes are written or the write op signals an error (r <= 0).
     *
     * For pipes, pipe_write_fn returns a positive partial count or a negative
     * errno (-EPIPE).  The loop handles both correctly:
     *   - partial positive: advance offset and retry the remainder.
     *   - zero or negative: break and return that value. */
    uint64_t total = 0;
    while (total < arg3) {
        int r = proc->fds[arg1].ops->write(
                    proc->fds[arg1].priv,
                    (const void *)(uintptr_t)(arg2 + total),
                    arg3 - total);
        if (r <= 0)
            return (total > 0) ? total : (uint64_t)(int64_t)r;
        total += (uint64_t)r;
    }
    return total;
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
uint64_t
sys_writev(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();

    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_WRITE, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

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

        /* Retry loop per iovec: console_write_fn returns a partial count when
         * the user buffer straddles a page boundary.  Loop until all bytes of
         * this vector are written or the write op signals an error. */
        uint64_t vec_written = 0;
        while (vec_written < iov.iov_len) {
            int r = proc->fds[arg1].ops->write(
                        proc->fds[arg1].priv,
                        (const void *)(uintptr_t)(iov.iov_base + vec_written),
                        iov.iov_len - vec_written);
            if (r <= 0) {
                if (r < 0 && total == 0)
                    return (uint64_t)(int64_t)r;
                goto done;
            }
            vec_written += (uint64_t)r;
        }
        total += vec_written;
    }
done:
    return total;
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
uint64_t
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
     * Pipe reads may block inside f->ops->read() via sched_block().
     * After unblocking, f->ops->read() returns the byte count and we
     * copy to user space and return normally via sysret.
     *
     * SYS_READ_BUF 4096: large enough for pipe PIPE_BUF_SIZE (4060) reads.
     * Stack budget: 4096 bytes here; sys_write path has ~4060 in pipe_write_fn.
     * These are separate syscall paths — they do not stack on each other.
     * Kernel stack is 16 KB; both are within budget. */
    #define SYS_READ_BUF 4096
    char kbuf[SYS_READ_BUF];
    /* Cap request to current page boundary so copy_to_user never crosses
     * into an unmapped page (e.g. the guard page just past USER_STACK_TOP).
     * Mirrors the same pattern in console_write_fn.  The caller (musl libc)
     * loops on short reads, so returning fewer bytes than requested is safe. */
    uint64_t page_off = arg2 & 0xFFFULL;
    uint64_t to_end   = 0x1000ULL - page_off;
    uint64_t n = arg3;
    if (n > SYS_READ_BUF) n = SYS_READ_BUF;
    if (n > to_end)       n = to_end;
    int64_t got = (int64_t)f->ops->read(f->priv, kbuf, f->offset, n);
    if (got < 0) return (uint64_t)got;   /* propagate -errno (e.g. -EPIPE) */
    if (got == 0) return 0;              /* clean EOF */
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
uint64_t
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
