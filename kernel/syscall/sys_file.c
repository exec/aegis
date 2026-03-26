/* sys_file.c — File and filesystem syscalls */
#include "sys_impl.h"

#ifndef TCGETS
#define TCGETS    0x5401UL
#define TCSETS    0x5402UL
#define TCSETSW   0x5403UL
#define TCSETSF   0x5404UL
#define TIOCSPGRP 0x5410UL
#endif

/*
 * sys_open — syscall 2
 *
 * arg1 = user pointer to null-terminated path string
 * arg2 = flags (ignored in Phase 10)
 * arg3 = mode (ignored in Phase 10)
 *
 * Returns fd on success, negative errno on failure.
 */
uint64_t
sys_open(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_OPEN, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    (void)arg3;   /* mode ignored */
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
    /* Resolve relative paths against proc->cwd.
     * "."       → cwd itself (e.g. "/")
     * "bin"     → cwd + "/" + "bin" (e.g. "/bin")
     * Absolute paths (start with '/') pass through unchanged. */
    if (kpath[0] != '/') {
        char resolved[256];
        uint64_t cwdlen = 0;
        while (proc->cwd[cwdlen]) cwdlen++;
        if (kpath[0] == '.' && kpath[1] == '\0') {
            /* "." — use cwd directly */
            __builtin_memcpy(resolved, proc->cwd, cwdlen + 1);
        } else {
            uint64_t pathlen = 0;
            while (kpath[pathlen]) pathlen++;
            /* Insert separator unless cwd already ends with '/' */
            uint64_t sep = (cwdlen > 0 && proc->cwd[cwdlen - 1] == '/') ? 0u : 1u;
            if (cwdlen + sep + pathlen >= 256)
                return (uint64_t)-(int64_t)36; /* ENAMETOOLONG */
            __builtin_memcpy(resolved, proc->cwd, cwdlen);
            if (sep) resolved[cwdlen] = '/';
            __builtin_memcpy(resolved + cwdlen + sep, kpath, pathlen + 1);
        }
        __builtin_memcpy(kpath, resolved, sizeof(kpath));
    }
    uint64_t fd;
    for (fd = 0; fd < PROC_MAX_FDS; fd++)
        if (!proc->fds[fd].ops) break;
    if (fd == PROC_MAX_FDS)
        return (uint64_t)-24;  /* EMFILE */

    /* /etc/shadow gate: requires CAP_KIND_AUTH regardless of uid.
     * There is no ambient root authority on Aegis — uid=0 is cosmetic.
     * Use memcmp against the known path — no libc strcmp available. */
    {
        static const char shadow_path[] = "/etc/shadow";
        int shadow_match = 1;
        for (uint64_t si = 0; si < sizeof(shadow_path); si++) {
            if (kpath[si] != shadow_path[si]) { shadow_match = 0; break; }
        }
        if (shadow_match) {
            if (cap_check(proc->caps, CAP_TABLE_SIZE,
                          CAP_KIND_AUTH, CAP_RIGHTS_READ) != 0)
                return (uint64_t)-(int64_t)13; /* EACCES */
        }
    }

    int r = vfs_open(kpath, (int)arg2, &proc->fds[fd]);
    if (r < 0)
        return (uint64_t)(int64_t)r;
    /* Store open flags in the fd slot for F_GETFL */
    proc->fds[fd].flags = (uint32_t)arg2;
    /* Propagate O_CLOEXEC from open flags to fd flags */
    if (arg2 & VFS_O_CLOEXEC)
        proc->fds[fd].flags |= VFS_FD_CLOEXEC;
    return fd;
}

/*
 * sys_openat — syscall 257
 *
 * arg1 = dirfd (AT_FDCWD = -100 means resolve relative to CWD)
 * arg2 = user pointer to path
 * arg3 = flags
 * arg4 = mode
 *
 * We only support AT_FDCWD (absolute paths and CWD-relative).  Paths starting
 * with '/' are absolute and dirfd is irrelevant.  For now we forward to
 * sys_open unconditionally — the shell only calls openat with AT_FDCWD.
 */
uint64_t
sys_openat(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    (void)arg1;  /* dirfd — only AT_FDCWD (-100) or absolute paths handled */
    return sys_open(arg2, arg3, arg4);
}

/* ── Phase 18 syscalls ───────────────────────────────────────────────────── */

int
stat_copy_path(uint64_t user_ptr, char *out, uint32_t bufsz)
{
    uint32_t i;
    for (i = 0; i < bufsz - 1; i++) {
        uint8_t c;
        if (!user_ptr_valid(user_ptr + i, 1))
            return -14; /* EFAULT */
        copy_from_user(&c, (const void *)(uintptr_t)(user_ptr + i), 1);
        out[i] = (char)c;
        if (c == 0) return 0;
    }
    out[bufsz - 1] = '\0';
    return 0;
}

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
uint64_t
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
uint64_t
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
 * Copies path to kernel buffer, validates it exists and is a directory
 * via vfs_stat_path, then updates proc->cwd.
 * Returns 0 on success, -EFAULT if pointer invalid, -ENOENT if not found,
 * -ENOTDIR if path exists but is not a directory.
 */
uint64_t
sys_chdir(uint64_t path_ptr)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (!user_ptr_valid(path_ptr, 1)) return (uint64_t)-(int64_t)14; /* EFAULT */

    char kpath[256];
    uint64_t i;
    for (i = 0; i < 255; i++) {
        char c;
        copy_from_user(&c, (const void *)(uintptr_t)(path_ptr + i), 1);
        kpath[i] = c;
        if (c == '\0') break;
    }
    kpath[255] = '\0';

    /* Validate path exists and is a directory */
    k_stat_t st;
    if (vfs_stat_path(kpath, &st) != 0) return (uint64_t)-(int64_t)2;  /* ENOENT */
    if ((st.st_mode & S_IFMT) != S_IFDIR) return (uint64_t)-(int64_t)20; /* ENOTDIR */

    for (i = 0; i < 256; i++) {
        proc->cwd[i] = kpath[i];
        if (kpath[i] == '\0') break;
    }
    return 0;
}

/*
 * sys_stat — syscall 4
 * arg1 = user pointer to path string (null-terminated, max 256 bytes)
 * arg2 = user pointer to struct stat output buffer
 */
uint64_t
sys_stat(uint64_t arg1, uint64_t arg2)
{
    char path[256];
    if (stat_copy_path(arg1, path, sizeof(path)) != 0)
        return (uint64_t)-(int64_t)14; /* EFAULT */

    k_stat_t ks;
    int rc = vfs_stat_path(path, &ks);
    if (rc != 0) return (uint64_t)-(int64_t)2; /* ENOENT */

    if (!user_ptr_valid(arg2, sizeof(ks)))
        return (uint64_t)-(int64_t)14; /* EFAULT */
    copy_to_user((void *)(uintptr_t)arg2, &ks, sizeof(ks));
    return 0;
}

/*
 * sys_fstat — syscall 5
 * arg1 = fd, arg2 = user pointer to struct stat output buffer
 */
uint64_t
sys_fstat(uint64_t arg1, uint64_t arg2)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (arg1 >= PROC_MAX_FDS) return (uint64_t)-(int64_t)9; /* EBADF */
    vfs_file_t *f = &proc->fds[arg1];
    if (!f->ops) return (uint64_t)-(int64_t)9; /* EBADF */

    k_stat_t ks;
    __builtin_memset(&ks, 0, sizeof(ks));

    if (f->ops->stat) {
        int rc = f->ops->stat(f->priv, &ks);
        if (rc != 0) return (uint64_t)-(int64_t)5; /* EIO */
    } else {
        /* Synthesize minimal stat for drivers without a stat hook. */
        ks.st_mode  = S_IFREG | 0444;
        ks.st_size  = (int64_t)f->size;
        ks.st_dev   = 1;
        ks.st_nlink = 1;
    }

    if (!user_ptr_valid(arg2, sizeof(ks)))
        return (uint64_t)-(int64_t)14; /* EFAULT */
    copy_to_user((void *)(uintptr_t)arg2, &ks, sizeof(ks));
    return 0;
}

/*
 * sys_access — syscall 21
 * arg1 = user pointer to path string, arg2 = mode (F_OK=0, R_OK=4, W_OK=2, X_OK=1)
 * Phase 18: return 0 if file exists, -ENOENT otherwise (no permission checks).
 */
uint64_t
sys_access(uint64_t arg1, uint64_t arg2)
{
    (void)arg2;
    char path[256];
    if (stat_copy_path(arg1, path, sizeof(path)) != 0)
        return (uint64_t)-(int64_t)14; /* EFAULT */
    k_stat_t ks;
    return (vfs_stat_path(path, &ks) == 0) ? 0 : (uint64_t)-(int64_t)2;
}

/*
 * sys_nanosleep — syscall 35
 * arg1 = user pointer to struct timespec { int64_t tv_sec; int64_t tv_nsec; }
 * arg2 = user pointer to remainder (NULL allowed; not populated)
 *
 * Uses sti; hlt; cli loop. PIT fires at 100 Hz (1 tick = 10ms).
 * Phase 18 limitation: starves other tasks (correct for single-process usage).
 */
uint64_t
sys_nanosleep(uint64_t arg1, uint64_t arg2)
{
    (void)arg2;
    struct { int64_t tv_sec; int64_t tv_nsec; } ts;
    if (!user_ptr_valid(arg1, sizeof(ts)))
        return (uint64_t)-(int64_t)14; /* EFAULT */
    copy_from_user(&ts, (const void *)(uintptr_t)arg1, sizeof(ts));

    uint64_t ticks = (uint64_t)ts.tv_sec * 100ULL
                   + (uint64_t)ts.tv_nsec / 10000000ULL;
    if (ticks == 0 && ts.tv_nsec > 0) ticks = 1;

    uint64_t deadline = arch_get_ticks() + ticks;
    while (arch_get_ticks() < deadline)
        arch_wait_for_irq();
    return 0;
}

/* Identity syscalls read from PCB. */
uint64_t sys_getuid(void) {
    aegis_process_t *p = (aegis_process_t *)sched_current();
    return p ? (uint64_t)p->uid : 0;
}
uint64_t sys_geteuid(void) { return sys_getuid(); }
uint64_t sys_getgid(void) {
    aegis_process_t *p = (aegis_process_t *)sched_current();
    return p ? (uint64_t)p->gid : 0;
}
uint64_t sys_getegid(void) { return sys_getgid(); }

/*
 * sys_ioctl — syscall 16
 *
 * arg1 = fd, arg2 = request, arg3 = arg (user pointer or value)
 *
 * TIOCGWINSZ (0x5413): return {rows=25, cols=80, 0, 0}
 * TIOCGPGRP  (0x540F): return foreground PID
 * FIONREAD   (0x541B): for pipes, return bytes available; others return 0
 * All others: return -ENOTTY
 */
uint64_t
sys_ioctl(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (arg1 >= PROC_MAX_FDS) return (uint64_t)-(int64_t)9; /* EBADF */
    vfs_file_t *f = &proc->fds[arg1];
    if (!f->ops) return (uint64_t)-(int64_t)9; /* EBADF */

    switch (arg2) {
    case 0x5413UL: { /* TIOCGWINSZ — struct winsize: uint16 rows, cols, xpixel, ypixel */
        uint16_t ws[4] = { 25, 80, 0, 0 };
        if (!user_ptr_valid(arg3, sizeof(ws)))
            return (uint64_t)-(int64_t)14; /* EFAULT */
        copy_to_user((void *)(uintptr_t)arg3, ws, sizeof(ws));
        return 0;
    }
    case 0x540FUL: { /* TIOCGPGRP — return foreground PID */
        uint32_t pgid = kbd_get_tty_pgrp();
        if (!user_ptr_valid(arg3, sizeof(pgid)))
            return (uint64_t)-(int64_t)14; /* EFAULT */
        copy_to_user((void *)(uintptr_t)arg3, &pgid, sizeof(pgid));
        return 0;
    }
    case 0x541BUL: { /* FIONREAD — bytes available in pipe */
        int32_t avail = 0;
        if (f->ops == &g_pipe_read_ops) {
            pipe_t *p = (pipe_t *)f->priv;
            avail = (int32_t)p->count;
        }
        if (!user_ptr_valid(arg3, sizeof(avail)))
            return (uint64_t)-(int64_t)14; /* EFAULT */
        copy_to_user((void *)(uintptr_t)arg3, &avail, sizeof(avail));
        return 0;
    }
    case TCGETS: {
        if (!kbd_vfs_is_tty(f))
            return (uint64_t)-(int64_t)25; /* ENOTTY */
        return (uint64_t)(int64_t)kbd_vfs_tcgets((void *)(uintptr_t)arg3);
    }
    case TCSETS:
    case TCSETSW:  /* drain is a no-op for our in-memory driver */
    case TCSETSF: { /* flush is a no-op */
        if (!kbd_vfs_is_tty(f))
            return (uint64_t)-(int64_t)25; /* ENOTTY */
        return (uint64_t)(int64_t)kbd_vfs_tcsets((const void *)(uintptr_t)arg3);
    }
    case TIOCSPGRP: {
        if (!kbd_vfs_is_tty(f))
            return (uint64_t)-(int64_t)25; /* ENOTTY */
        if (!user_ptr_valid(arg3, sizeof(uint32_t)))
            return (uint64_t)-(int64_t)14; /* EFAULT */
        uint32_t pgid;
        copy_from_user(&pgid, (const void *)(uintptr_t)arg3, sizeof(uint32_t));
        kbd_set_tty_pgrp(pgid);
        return 0;
    }
    default:
        return (uint64_t)-(int64_t)25; /* ENOTTY */
    }
}

/*
 * sys_fcntl — syscall 72
 *
 * arg1 = fd, arg2 = cmd, arg3 = arg
 *
 * F_GETFL (3): return f->flags
 * F_SETFL (4): store arg & O_NONBLOCK into f->flags (O_NONBLOCK=0x800)
 * F_GETFD (1): return FD_CLOEXEC (1) if set, 0 otherwise
 * F_SETFD (2): set or clear FD_CLOEXEC based on arg3 bit 0
 * F_DUPFD (0): find lowest fd >= arg, dup into it
 * All others: return -EINVAL
 */
uint64_t
sys_fcntl(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (arg1 >= PROC_MAX_FDS) return (uint64_t)-(int64_t)9; /* EBADF */
    vfs_file_t *f = &proc->fds[arg1];
    if (!f->ops) return (uint64_t)-(int64_t)9; /* EBADF */

    switch (arg2) {
    case 1: /* F_GETFD — return FD_CLOEXEC (1) if set, 0 otherwise */
        return (proc->fds[arg1].flags & VFS_FD_CLOEXEC) ? 1 : 0;
    case 2: /* F_SETFD — set or clear FD_CLOEXEC based on arg3 bit 0 (FD_CLOEXEC=1) */
        if (arg3 & 1)
            proc->fds[arg1].flags |= VFS_FD_CLOEXEC;
        else
            proc->fds[arg1].flags &= ~VFS_FD_CLOEXEC;
        return 0;
    case 3: /* F_GETFL */ return (uint64_t)f->flags;
    case 4: /* F_SETFL */
        f->flags = (f->flags & ~0x800U) | ((uint32_t)arg3 & 0x800U);
        /* Also update socket nonblocking flag if fd is a socket */
        {
            uint32_t sid2 = sock_id_from_fd((int)arg1, proc);
            if (sid2 != SOCK_NONE) {
                sock_t *sk = sock_get(sid2);
                if (sk)
                    sk->nonblocking = (arg3 & 0x800U) ? 1 : 0;
            }
        }
        return 0;
    case 0:   /* F_DUPFD */
    case 1030: { /* F_DUPFD_CLOEXEC (0x406) — same as F_DUPFD + set FD_CLOEXEC */
        uint32_t new_fd;
        for (new_fd = (uint32_t)arg3; new_fd < PROC_MAX_FDS; new_fd++) {
            if (!proc->fds[new_fd].ops) break;
        }
        if (new_fd >= PROC_MAX_FDS) return (uint64_t)-(int64_t)24; /* EMFILE */
        proc->fds[new_fd] = *f; /* struct copy */
        proc->fds[new_fd].flags &= ~VFS_FD_CLOEXEC;   /* clear first */
        if (arg2 == 1030) proc->fds[new_fd].flags |= VFS_FD_CLOEXEC;
        if (f->ops->dup) f->ops->dup(f->priv);
        return (uint64_t)new_fd;
    }
    default:
        return (uint64_t)-(int64_t)22; /* EINVAL */
    }
}

/*
 * sys_lseek — syscall 8
 *
 * arg1 = fd, arg2 = offset, arg3 = whence (SEEK_SET=0, SEEK_CUR=1, SEEK_END=2)
 *
 * For non-seekable fds (pipes, console, kbd): return -ESPIPE (-29).
 * For initrd files: update f->offset accordingly and return new offset.
 * Kernel uses f->offset for position tracking; lseek must keep it consistent.
 */
uint64_t
sys_lseek(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (arg1 >= PROC_MAX_FDS || !proc->fds[arg1].ops)
        return (uint64_t)-9;   /* EBADF */
    vfs_file_t *f = &proc->fds[arg1];

    /* Non-seekable fd (no size): return ESPIPE */
    if (f->size == 0)
        return (uint64_t)-(int64_t)29;   /* -ESPIPE */

    int64_t new_off;
    int64_t off = (int64_t)arg2;
    if (arg3 == 0)        /* SEEK_SET */
        new_off = off;
    else if (arg3 == 1) { /* SEEK_CUR */
        /* S3: Safe signed overflow checks for SEEK_CUR.
         * The old guards used 0x8000000000000000LL which is UB. */
        int64_t cur = (int64_t)f->offset;
        if (off > 0 && cur > (int64_t)0x7FFFFFFFFFFFFFFFLL - off)
            return (uint64_t)(int64_t)-22;  /* -EINVAL */
        if (off < 0 && cur < (int64_t)(-0x7FFFFFFFFFFFFFFFLL - 1) - off)
            return (uint64_t)(int64_t)-22;  /* -EINVAL */
        new_off = (int64_t)f->offset + off;
    } else if (arg3 == 2) { /* SEEK_END */
        /* Guard against signed overflow: f->size is uint64_t.
         * Reject if f->size itself exceeds INT64_MAX, or if adding a positive
         * off would push the result past INT64_MAX. */
        if (f->size > (uint64_t)0x7FFFFFFFFFFFFFFFLL ||
            (off > 0 && (int64_t)f->size > (int64_t)0x7FFFFFFFFFFFFFFFLL - off))
            return (uint64_t)-(int64_t)22;   /* EINVAL */
        new_off = (int64_t)f->size + off;
    } else
        return (uint64_t)-(int64_t)22;   /* EINVAL */

    if (new_off < 0)
        return (uint64_t)-(int64_t)22;   /* EINVAL */
    f->offset = (uint64_t)new_off;
    return (uint64_t)new_off;
}

/*
 * sys_pipe2 — syscall 293
 *
 * arg1 = user pointer to int[2] — receives [read_fd, write_fd]
 * arg2 = flags (O_CLOEXEC = 0x80000 supported; stored in fd flags via VFS_FD_CLOEXEC)
 *
 * Allocates a pipe_t from kva, installs read and write ends into two free
 * fd slots, writes the fd numbers to user pipefd.
 */
uint64_t
sys_pipe2(uint64_t arg1, uint64_t arg2)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    uint32_t pipe_flags = (uint32_t)arg2;

    if (!user_ptr_valid(arg1, 2 * sizeof(int)))
        return (uint64_t)-14;   /* EFAULT */

    /* Find two free fd slots */
    int rfd = -1, wfd = -1;
    int i;
    for (i = 0; i < PROC_MAX_FDS; i++) {
        if (!proc->fds[i].ops) {
            if (rfd < 0) { rfd = i; }
            else         { wfd = i; break; }
        }
    }
    if (wfd < 0)
        return (uint64_t)-24;   /* EMFILE */

    /* Allocate and zero-initialize pipe_t (exactly one kva page) */
    pipe_t *p = kva_alloc_pages(1);
    if (!p)
        return (uint64_t)-12;   /* ENOMEM */
    __builtin_memset(p, 0, sizeof(pipe_t));
    p->read_refs  = 1;
    p->write_refs = 1;

    /* Install read end */
    proc->fds[rfd].ops    = &g_pipe_read_ops;
    proc->fds[rfd].priv   = p;
    proc->fds[rfd].offset = 0;
    proc->fds[rfd].size   = 0;
    proc->fds[rfd].flags  = 0;

    /* Install write end */
    proc->fds[wfd].ops    = &g_pipe_write_ops;
    proc->fds[wfd].priv   = p;
    proc->fds[wfd].offset = 0;
    proc->fds[wfd].size   = 0;
    proc->fds[wfd].flags  = 0;

    /* Propagate O_CLOEXEC to both pipe ends */
    if (pipe_flags & VFS_O_CLOEXEC) {
        proc->fds[rfd].flags |= VFS_FD_CLOEXEC;
        proc->fds[wfd].flags |= VFS_FD_CLOEXEC;
    }

    /* Write [rfd, wfd] to user pipefd array */
    int out[2] = { rfd, wfd };
    copy_to_user((void *)(uintptr_t)arg1, out, sizeof(out));

    return 0;
}

/*
 * sys_dup — syscall 32
 *
 * arg1 = oldfd
 *
 * Duplicates oldfd into the lowest free fd slot. Calls the dup hook
 * to increment any driver-side reference counts (e.g., pipe_t refs).
 */
uint64_t
sys_dup(uint64_t arg1)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (arg1 >= PROC_MAX_FDS || !proc->fds[arg1].ops)
        return (uint64_t)-9;    /* EBADF */

    int newfd = -1;
    int i;
    for (i = 0; i < PROC_MAX_FDS; i++) {
        if (!proc->fds[i].ops) { newfd = i; break; }
    }
    if (newfd < 0)
        return (uint64_t)-24;   /* EMFILE */

    /* Copy fd struct by value, then bump refcount via dup hook. */
    proc->fds[newfd] = proc->fds[arg1];
    proc->fds[newfd].flags &= ~VFS_FD_CLOEXEC;  /* POSIX: dup clears FD_CLOEXEC */
    if (proc->fds[newfd].ops->dup)
        proc->fds[newfd].ops->dup(proc->fds[newfd].priv);

    return (uint64_t)newfd;
}

/*
 * sys_dup2 — syscall 33
 *
 * arg1 = oldfd, arg2 = newfd
 *
 * Duplicates oldfd into newfd. Closes newfd first if it is open.
 * If oldfd == newfd, returns newfd with no changes.
 */
uint64_t
sys_dup2(uint64_t arg1, uint64_t arg2)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (arg1 >= PROC_MAX_FDS || !proc->fds[arg1].ops)
        return (uint64_t)-9;    /* EBADF */
    if (arg2 >= PROC_MAX_FDS)
        return (uint64_t)-9;    /* EBADF */
    if (arg1 == arg2)
        return arg2;            /* no-op per POSIX */

    /* Close existing target fd */
    if (proc->fds[arg2].ops) {
        proc->fds[arg2].ops->close(proc->fds[arg2].priv);
        __builtin_memset(&proc->fds[arg2], 0, sizeof(vfs_file_t));
    }

    /* Copy fd struct by value, then bump refcount via dup hook. */
    proc->fds[arg2] = proc->fds[arg1];
    proc->fds[arg2].flags &= ~VFS_FD_CLOEXEC;  /* POSIX: dup2 clears FD_CLOEXEC */
    if (proc->fds[arg2].ops->dup)
        proc->fds[arg2].ops->dup(proc->fds[arg2].priv);

    return arg2;
}

/*
 * copy_path_from_user — copy a null-terminated path string from user space.
 *
 * Uses the same byte-by-byte pattern as sys_open to avoid crossing unmapped
 * pages near the top of the user stack.  Returns 0 on success, -14 (EFAULT)
 * if any byte is in kernel space.
 */
int
copy_path_from_user(char *kpath, uint64_t user_ptr, uint32_t bufsz)
{
    uint32_t i;
    for (i = 0; i < bufsz - 1; i++) {
        if (!user_ptr_valid(user_ptr + i, 1))
            return -14; /* EFAULT */
        char c;
        copy_from_user(&c, (const void *)(uintptr_t)(user_ptr + i), 1);
        kpath[i] = c;
        if (c == '\0') return 0;
    }
    kpath[bufsz - 1] = '\0';
    return 0;
}

/*
 * sys_mkdir — syscall 83
 *
 * arg1 = user pointer to null-terminated path string
 * arg2 = mode (ignored for now)
 *
 * Returns 0 on success, negative errno on failure.
 */
uint64_t
sys_mkdir(uint64_t arg1, uint64_t arg2)
{
    char kpath[256];
    (void)arg2; /* mode ignored for now */
    if (copy_path_from_user(kpath, arg1, sizeof(kpath)) != 0)
        return (uint64_t)-(int64_t)14; /* EFAULT */
    int r = ext2_mkdir(kpath, 0755);
    return (r < 0) ? (uint64_t)(int64_t)r : 0;
}

/*
 * sys_unlink — syscall 87
 *
 * arg1 = user pointer to null-terminated path string
 *
 * Returns 0 on success, negative errno on failure.
 */
uint64_t
sys_unlink(uint64_t arg1)
{
    char kpath[256];
    if (copy_path_from_user(kpath, arg1, sizeof(kpath)) != 0)
        return (uint64_t)-(int64_t)14; /* EFAULT */
    int r = ext2_unlink(kpath);
    return (r < 0) ? (uint64_t)(int64_t)r : 0;
}

/*
 * sys_rename — syscall 82
 *
 * arg1 = user pointer to null-terminated old path
 * arg2 = user pointer to null-terminated new path
 *
 * Returns 0 on success, negative errno on failure.
 */
uint64_t
sys_rename(uint64_t arg1, uint64_t arg2)
{
    char kold[256], knew[256];
    if (copy_path_from_user(kold, arg1, sizeof(kold)) != 0)
        return (uint64_t)-(int64_t)14; /* EFAULT */
    if (copy_path_from_user(knew, arg2, sizeof(knew)) != 0)
        return (uint64_t)-(int64_t)14; /* EFAULT */
    int r = ext2_rename(kold, knew);
    return (r < 0) ? (uint64_t)(int64_t)r : 0;
}

/*
 * sys_sync — syscall 162
 *
 * Flush all dirty ext2 blocks to disk.
 * Matches POSIX sync(2): no arguments, no return value besides 0.
 */
uint64_t
sys_sync(void)
{
    ext2_sync();
    return 0;
}

/*
 * sys_clock_gettime — syscall 228
 *
 * arg1 = clk_id  (CLOCK_REALTIME=0, CLOCK_MONOTONIC=1; both return PIT ticks)
 * arg2 = user pointer to struct timespec { int64_t tv_sec; int64_t tv_nsec; }
 *
 * Returns 0 on success, negative errno on failure.
 */
uint64_t
sys_clock_gettime(uint64_t clk_id, uint64_t timespec_uptr)
{
    if (clk_id != 0 && clk_id != 1) return (uint64_t)-(int64_t)22; /* EINVAL */
    if (!user_ptr_valid(timespec_uptr, 16)) return (uint64_t)-(int64_t)14; /* EFAULT */
    uint64_t ticks = arch_get_ticks();
    int64_t tv_sec  = (int64_t)(ticks / 100ULL);
    int64_t tv_nsec = (int64_t)((ticks % 100ULL) * 10000000ULL);
    copy_to_user((void *)(uintptr_t)timespec_uptr,       &tv_sec,  8);
    copy_to_user((void *)(uintptr_t)(timespec_uptr + 8), &tv_nsec, 8);
    return 0;
}
