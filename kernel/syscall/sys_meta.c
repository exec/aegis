/* sys_meta.c — File metadata syscalls: lstat, symlink, readlink, chmod, chown */
#include "sys_impl.h"
#include "sched.h"
#include "proc.h"
#include "vfs.h"
#include "ext2.h"

/* ── Helper: resolve relative path against cwd ──────────────────────── */

static int
resolve_path(const char *kpath, const char *cwd, char *out, uint32_t outsz)
{
    if (kpath[0] == '/') {
        uint32_t i;
        for (i = 0; i < outsz - 1 && kpath[i]; i++)
            out[i] = kpath[i];
        out[i] = '\0';
        return 0;
    }
    uint32_t cwdlen = 0;
    while (cwd[cwdlen]) cwdlen++;
    uint32_t pathlen = 0;
    while (kpath[pathlen]) pathlen++;
    uint32_t sep = (cwdlen > 0 && cwd[cwdlen - 1] == '/') ? 0u : 1u;
    if (cwdlen + sep + pathlen >= outsz)
        return -36; /* ENAMETOOLONG */
    __builtin_memcpy(out, cwd, cwdlen);
    if (sep) out[cwdlen] = '/';
    __builtin_memcpy(out + cwdlen + sep, kpath, pathlen + 1);
    return 0;
}

/*
 * sys_lstat — syscall 6
 * Like sys_stat but does not follow symlinks on the final component.
 */
uint64_t
sys_lstat(uint64_t arg1, uint64_t arg2)
{
    char path[256];
    if (stat_copy_path(arg1, path, sizeof(path)) != 0)
        return (uint64_t)-(int64_t)14; /* EFAULT */

    k_stat_t ks;
    int rc = vfs_stat_path_ex(path, &ks, 0);
    if (rc != 0) return (uint64_t)-(int64_t)2; /* ENOENT */

    if (!user_ptr_valid(arg2, sizeof(ks)))
        return (uint64_t)-(int64_t)14; /* EFAULT */
    copy_to_user((void *)(uintptr_t)arg2, &ks, sizeof(ks));
    return 0;
}

/*
 * sys_symlink — syscall 88
 * arg1 = user pointer to target string (stored as-is)
 * arg2 = user pointer to linkpath string (resolved against cwd)
 */
uint64_t
sys_symlink(uint64_t arg1, uint64_t arg2)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_WRITE, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    char target[256], linkpath[256], resolved[256];
    if (copy_path_from_user(target, arg1, sizeof(target)) != 0)
        return (uint64_t)-(int64_t)14; /* EFAULT */
    if (copy_path_from_user(linkpath, arg2, sizeof(linkpath)) != 0)
        return (uint64_t)-(int64_t)14; /* EFAULT */

    /* Resolve linkpath against cwd (target is stored as-is) */
    if (resolve_path(linkpath, proc->cwd, resolved, sizeof(resolved)) != 0)
        return (uint64_t)-(int64_t)36; /* ENAMETOOLONG */

    int r = ext2_symlink(resolved, target);
    return (r < 0) ? (uint64_t)(int64_t)r : 0;
}

/*
 * sys_readlink — syscall 89
 * arg1 = user pointer to path string
 * arg2 = user pointer to output buffer
 * arg3 = buffer size
 */
uint64_t
sys_readlink(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_READ, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    char path[256], resolved[256];
    if (copy_path_from_user(path, arg1, sizeof(path)) != 0)
        return (uint64_t)-(int64_t)14; /* EFAULT */

    if (resolve_path(path, proc->cwd, resolved, sizeof(resolved)) != 0)
        return (uint64_t)-(int64_t)36; /* ENAMETOOLONG */

    char kbuf[256];
    uint32_t bufsiz = (uint32_t)arg3;
    if (bufsiz > sizeof(kbuf)) bufsiz = sizeof(kbuf);

    int n = ext2_readlink(resolved, kbuf, bufsiz);
    if (n < 0) return (uint64_t)(int64_t)n;

    if (!user_ptr_valid(arg2, (uint64_t)n))
        return (uint64_t)-(int64_t)14; /* EFAULT */
    copy_to_user((void *)(uintptr_t)arg2, kbuf, (uint32_t)n);
    return (uint64_t)n;
}

/*
 * sys_chmod — syscall 90
 * arg1 = user pointer to path string
 * arg2 = mode (permission bits)
 */
uint64_t
sys_chmod(uint64_t arg1, uint64_t arg2)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_WRITE, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    char path[256], resolved[256];
    if (copy_path_from_user(path, arg1, sizeof(path)) != 0)
        return (uint64_t)-(int64_t)14; /* EFAULT */

    if (resolve_path(path, proc->cwd, resolved, sizeof(resolved)) != 0)
        return (uint64_t)-(int64_t)36; /* ENAMETOOLONG */

    /* Ownership check: only file owner (or uid 0) may chmod */
    {
        uint32_t ino;
        if (ext2_open(resolved, &ino) == 0) {
            ext2_inode_t inode;
            if (ext2_read_inode(ino, &inode) == 0) {
                if (proc->uid != 0 && proc->uid != inode.i_uid)
                    return (uint64_t)-(int64_t)13; /* EACCES */
            }
        }
    }

    int r = ext2_chmod(resolved, (uint16_t)arg2);
    return (r < 0) ? (uint64_t)(int64_t)r : 0;
}

/*
 * sys_fchmod — syscall 91
 * arg1 = fd, arg2 = mode (permission bits)
 */
uint64_t
sys_fchmod(uint64_t arg1, uint64_t arg2)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_WRITE, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    if (arg1 >= PROC_MAX_FDS) return (uint64_t)-(int64_t)9; /* EBADF */
    vfs_file_t *f = &proc->fd_table->fds[arg1];
    if (!f->ops) return (uint64_t)-(int64_t)9; /* EBADF */

    /* Ownership check via stat: only file owner (or uid 0) may fchmod */
    if (f->ops->stat) {
        k_stat_t ks;
        if (f->ops->stat(f->priv, &ks) == 0) {
            if (proc->uid != 0 && proc->uid != ks.st_uid)
                return (uint64_t)-(int64_t)13; /* EACCES */
        }
    }

    int r = vfs_fchmod(f, (uint16_t)arg2);
    if (r < 0) return (uint64_t)-(int64_t)22; /* EINVAL — not an ext2 fd */
    return 0;
}

/*
 * sys_chown — syscall 92
 * arg1 = user pointer to path, arg2 = uid, arg3 = gid
 * Follows symlinks.
 */
uint64_t
sys_chown(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_SETUID, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    char path[256], resolved[256];
    if (copy_path_from_user(path, arg1, sizeof(path)) != 0)
        return (uint64_t)-(int64_t)14; /* EFAULT */

    if (resolve_path(path, proc->cwd, resolved, sizeof(resolved)) != 0)
        return (uint64_t)-(int64_t)36; /* ENAMETOOLONG */

    /* Ownership check: only file owner (or uid 0) may chown */
    {
        uint32_t ino;
        if (ext2_open(resolved, &ino) == 0) {
            ext2_inode_t inode;
            if (ext2_read_inode(ino, &inode) == 0) {
                if (proc->uid != 0 && proc->uid != inode.i_uid)
                    return (uint64_t)-(int64_t)13; /* EACCES */
            }
        }
    }

    int r = ext2_chown(resolved, (uint16_t)arg2, (uint16_t)arg3, 1);
    return (r < 0) ? (uint64_t)(int64_t)r : 0;
}

/*
 * sys_fchown — syscall 93
 * arg1 = fd, arg2 = uid, arg3 = gid
 */
uint64_t
sys_fchown(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_SETUID, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    if (arg1 >= PROC_MAX_FDS) return (uint64_t)-(int64_t)9; /* EBADF */
    vfs_file_t *f = &proc->fd_table->fds[arg1];
    if (!f->ops) return (uint64_t)-(int64_t)9; /* EBADF */

    /* Ownership check via stat: only file owner (or uid 0) may fchown */
    if (f->ops->stat) {
        k_stat_t ks;
        if (f->ops->stat(f->priv, &ks) == 0) {
            if (proc->uid != 0 && proc->uid != ks.st_uid)
                return (uint64_t)-(int64_t)13; /* EACCES */
        }
    }

    int r = vfs_fchown(f, (uint16_t)arg2, (uint16_t)arg3);
    if (r < 0) return (uint64_t)-(int64_t)22; /* EINVAL — not an ext2 fd */
    return 0;
}

/*
 * sys_lchown — syscall 94
 * Like sys_chown but does not follow symlinks.
 */
uint64_t
sys_lchown(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_SETUID, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    char path[256], resolved[256];
    if (copy_path_from_user(path, arg1, sizeof(path)) != 0)
        return (uint64_t)-(int64_t)14; /* EFAULT */

    if (resolve_path(path, proc->cwd, resolved, sizeof(resolved)) != 0)
        return (uint64_t)-(int64_t)36; /* ENAMETOOLONG */

    /* Ownership check: only file owner (or uid 0) may lchown */
    {
        uint32_t ino;
        if (ext2_open(resolved, &ino) == 0) {
            ext2_inode_t inode;
            if (ext2_read_inode(ino, &inode) == 0) {
                if (proc->uid != 0 && proc->uid != inode.i_uid)
                    return (uint64_t)-(int64_t)13; /* EACCES */
            }
        }
    }

    int r = ext2_chown(resolved, (uint16_t)arg2, (uint16_t)arg3, 0);
    return (r < 0) ? (uint64_t)(int64_t)r : 0;
}
