/* sys_dir.c — Directory syscalls: getdents64, mkdir, unlink, rename */
#include "sys_impl.h"
#include "sched.h"
#include "proc.h"
#include "vfs.h"
#include "ext2.h"

/*
 * sys_getdents64 — syscall 217
 *
 * fd_num = file descriptor for a directory
 * dirp   = user pointer to output buffer
 * count  = buffer size in bytes
 *
 * Returns number of bytes written on success, 0 at end, negative errno on failure.
 */
uint64_t
sys_getdents64(uint64_t fd_num, uint64_t dirp, uint64_t count)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (fd_num >= PROC_MAX_FDS) return (uint64_t)-(int64_t)EBADF;
    vfs_file_t *f = &proc->fd_table->fds[fd_num];
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
        while (name[namelen] && namelen < 255) namelen++;
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
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_WRITE, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)1; /* EPERM */
    char kpath[256];
    (void)arg2; /* mode ignored for now */
    if (copy_path_from_user(kpath, arg1, sizeof(kpath)) != 0)
        return (uint64_t)-(int64_t)14; /* EFAULT */
    /* Check W+X permission on parent directory */
    {
        uint32_t parent_ino;
        const char *bname;
        if (ext2_lookup_parent(kpath, &parent_ino, &bname) == 0) {
            int pperm = ext2_check_perm(parent_ino,
                (uint16_t)proc->uid, (uint16_t)proc->gid, 2 | 1);
            if (pperm != 0)
                return (uint64_t)(-13);  /* EACCES */
        }
    }
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
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_WRITE, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)1; /* EPERM */
    char kpath[256];
    if (copy_path_from_user(kpath, arg1, sizeof(kpath)) != 0)
        return (uint64_t)-(int64_t)14; /* EFAULT */
    /* Check W+X permission on parent directory */
    {
        uint32_t parent_ino;
        const char *bname;
        if (ext2_lookup_parent(kpath, &parent_ino, &bname) == 0) {
            int pperm = ext2_check_perm(parent_ino,
                (uint16_t)proc->uid, (uint16_t)proc->gid, 2 | 1);
            if (pperm != 0)
                return (uint64_t)(-13);  /* EACCES */
        }
    }
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
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_WRITE, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)1; /* EPERM */
    char kold[256], knew[256];
    if (copy_path_from_user(kold, arg1, sizeof(kold)) != 0)
        return (uint64_t)-(int64_t)14; /* EFAULT */
    if (copy_path_from_user(knew, arg2, sizeof(knew)) != 0)
        return (uint64_t)-(int64_t)14; /* EFAULT */
    /* Check W+X permission on both source and destination parent dirs */
    {
        uint32_t parent_ino;
        const char *bname;
        if (ext2_lookup_parent(kold, &parent_ino, &bname) == 0) {
            int pperm = ext2_check_perm(parent_ino,
                (uint16_t)proc->uid, (uint16_t)proc->gid, 2 | 1);
            if (pperm != 0)
                return (uint64_t)(-13);  /* EACCES */
        }
        if (ext2_lookup_parent(knew, &parent_ino, &bname) == 0) {
            int pperm = ext2_check_perm(parent_ino,
                (uint16_t)proc->uid, (uint16_t)proc->gid, 2 | 1);
            if (pperm != 0)
                return (uint64_t)(-13);  /* EACCES */
        }
    }
    int r = ext2_rename(kold, knew);
    return (r < 0) ? (uint64_t)(int64_t)r : 0;
}
