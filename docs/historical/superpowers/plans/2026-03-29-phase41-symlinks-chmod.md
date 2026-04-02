# Phase 41: Symlinks + chmod/chown Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add POSIX symlink support and file permission enforcement to Aegis ext2, with 7 new syscalls and DAC checking at VFS open/access/execve/unlink/mkdir/rename.

**Architecture:** ext2 gains symlink inode creation/reading/following inside its path walk. A new `ext2_check_perm()` function enforces mode bits against process uid/gid at each enforcement point. VFS gains a `follow` flag for lstat. 7 new syscalls (symlink/readlink/chmod/fchmod/chown/fchown/lchown) are wired into `syscall_dispatch`. Minimal user-space tools (`ln`, `chmod`, `chown`, `readlink`) built for testing.

**Tech Stack:** C (kernel), Python (test harness), musl-gcc (user tools)

---

### Task 1: ext2 constants + `ext2_check_perm` + `ext2_read_symlink_target`

**Files:**
- Modify: `kernel/fs/ext2.h` (add constants + new function prototypes)
- Modify: `kernel/fs/ext2.c` (add `ext2_check_perm`, `ext2_read_symlink_target`, `ext2_symlink`, `ext2_readlink`, `ext2_chmod`, `ext2_chown`)
- Modify: `kernel/fs/ext2_internal.h` (add ELOOP, EACCES, EINVAL errnos)
- Modify: `kernel/fs/vfs.h` (add `S_IFLNK` constant)

- [ ] **Step 1: Add constants to ext2.h**

Add after line 108 (`#define EXT2_S_IFDIR  0x4000`):

```c
#define EXT2_S_IFLNK  0xA000

/* Directory entry file type for symlinks */
#define EXT2_FT_SYMLINK  7

/* Maximum symlink resolution depth */
#define SYMLINK_MAX_DEPTH 8
```

Add after line 128 (after `ext2_is_dir` prototype):

```c
/* Symlink operations */
int ext2_symlink(const char *linkpath, const char *target);
int ext2_readlink(const char *path, char *buf, uint32_t bufsiz);
int ext2_read_symlink_target(uint32_t ino, char *buf, uint32_t bufsiz);

/* Permission and metadata operations */
int ext2_check_perm(uint32_t ino, uint16_t proc_uid, uint16_t proc_gid, int want);
int ext2_chmod(const char *path, uint16_t mode);
int ext2_chown(const char *path, uint16_t uid, uint16_t gid, int follow);

/* Path resolution with symlink following control */
int ext2_open_ex(const char *path, uint32_t *inode_out, int follow_final);
```

- [ ] **Step 2: Add S_IFLNK to vfs.h**

Add after line 14 (`#define S_IFIFO  0010000U`):

```c
#define S_IFLNK  0120000U  /* symbolic link */
```

- [ ] **Step 3: Add errno constants to ext2_internal.h**

Add after line 13 (`#define ENAMETOOLONG 36`):

```c
#define EACCES       13
#define EINVAL       22
#define ELOOP        40
```

- [ ] **Step 4: Implement `ext2_check_perm` in ext2.c**

Add after the `ext2_is_dir` function (after line 486):

```c
/* ------------------------------------------------------------------ */
/* ext2_check_perm — POSIX DAC check against inode mode bits           */
/* ------------------------------------------------------------------ */

int ext2_check_perm(uint32_t ino, uint16_t proc_uid, uint16_t proc_gid, int want)
{
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0)
        return -EIO;

    uint16_t mode = inode.i_mode;
    uint16_t perm;

    /* No root bypass — Aegis has no ambient authority */
    if (proc_uid == inode.i_uid)
        perm = (mode >> 6) & 7;   /* owner bits */
    else if (proc_gid == inode.i_gid)
        perm = (mode >> 3) & 7;   /* group bits */
    else
        perm = mode & 7;          /* other bits */

    if ((perm & want) == (uint16_t)want)
        return 0;
    return -EACCES;
}
```

- [ ] **Step 5: Implement `ext2_read_symlink_target` in ext2.c**

Add after `ext2_check_perm`:

```c
/* ------------------------------------------------------------------ */
/* ext2_read_symlink_target — read symlink target from inode           */
/* ------------------------------------------------------------------ */

int ext2_read_symlink_target(uint32_t ino, char *buf, uint32_t bufsiz)
{
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0)
        return -EIO;

    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFLNK)
        return -EINVAL;

    uint32_t len = inode.i_size;
    if (len > bufsiz)
        len = bufsiz;

    if (inode.i_size <= 60) {
        /* Fast symlink: target stored in i_block[] */
        const uint8_t *src = (const uint8_t *)inode.i_block;
        uint32_t i;
        for (i = 0; i < len; i++)
            buf[i] = (char)src[i];
    } else {
        /* Slow symlink: target in data block i_block[0] */
        uint32_t blk = inode.i_block[0];
        if (blk == 0)
            return -EIO;
        uint8_t *data = cache_get_slot(blk);
        if (!data)
            return -EIO;
        uint32_t i;
        for (i = 0; i < len; i++)
            buf[i] = (char)data[i];
    }
    return (int)len;
}
```

- [ ] **Step 6: Implement `ext2_open_ex` — path walk with symlink following**

Replace the existing `ext2_open` (lines 228-333) with `ext2_open_ex` that adds symlink following, then make `ext2_open` a thin wrapper:

```c
/* ------------------------------------------------------------------ */
/* ext2_open_ex — walk path from root inode 2 with symlink support     */
/* ------------------------------------------------------------------ */

int ext2_open_ex(const char *path, uint32_t *inode_out, int follow_final)
{
    if (!s_mounted)
        return -1;

    uint32_t current_ino = EXT2_ROOT_INODE;
    int symlink_depth = 0;

    /* Work with a mutable copy of the path for symlink substitution */
    char resolved[512];
    uint32_t ri;
    for (ri = 0; ri < 511 && path[ri]; ri++)
        resolved[ri] = path[ri];
    resolved[ri] = '\0';

    const char *p = resolved;

    /* skip leading slashes */
    while (*p == '/')
        p++;

    /* If path is empty (root dir itself) */
    if (*p == '\0') {
        *inode_out = current_ino;
        return 0;
    }

restart_walk:
    while (*p != '\0') {
        /* Extract next component */
        char component[256];
        uint32_t clen = 0;
        while (*p != '\0' && *p != '/') {
            if (clen < 255)
                component[clen++] = *p;
            p++;
        }
        component[clen] = '\0';

        /* Skip trailing slashes */
        while (*p == '/')
            p++;

        int is_final = (*p == '\0');

        /* ".." → clamp to root */
        if (component[0] == '.' && component[1] == '.' && component[2] == '\0') {
            current_ino = EXT2_ROOT_INODE;
            continue;
        }
        /* "." → stay in current dir */
        if (component[0] == '.' && component[1] == '\0')
            continue;

        /* Read current directory inode */
        ext2_inode_t dir_inode;
        if (ext2_read_inode(current_ino, &dir_inode) < 0)
            return -1;

        /* Must be a directory */
        if ((dir_inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
            return -1;

        /* Search directory entries for component */
        uint32_t parent_ino = current_ino;
        int found = 0;
        uint32_t pos = 0;

        while (pos < dir_inode.i_size) {
            uint32_t file_block = pos / s_block_size;
            uint32_t blk = ext2_block_num(&dir_inode, file_block);
            if (blk == 0)
                break;
            uint8_t *data = cache_get_slot(blk);
            if (!data)
                return -1;
            uint32_t block_pos = pos % s_block_size;
            while (block_pos < s_block_size) {
                ext2_dirent_t *de = (ext2_dirent_t *)(data + block_pos);
                if (de->rec_len < 8 || block_pos + de->rec_len > s_block_size)
                    break;
                if (de->inode == 0) {
                    block_pos += de->rec_len;
                    pos += de->rec_len;
                    continue;
                }
                if (de->name_len == (uint8_t)clen) {
                    uint32_t k;
                    int match = 1;
                    for (k = 0; k < clen; k++) {
                        if (de->name[k] != component[k]) {
                            match = 0;
                            break;
                        }
                    }
                    if (match) {
                        current_ino = de->inode;
                        found = 1;
                        break;
                    }
                }
                block_pos += de->rec_len;
                pos += de->rec_len;
            }
            if (found)
                break;
            if (!found) {
                uint32_t block_end = (file_block + 1) * s_block_size;
                if (pos < block_end)
                    pos = block_end;
            }
        }

        if (!found)
            return -1;

        /* Check if resolved inode is a symlink */
        ext2_inode_t child_inode;
        if (ext2_read_inode(current_ino, &child_inode) < 0)
            return -1;

        if ((child_inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK) {
            /* Final component + no-follow → return symlink inode itself */
            if (is_final && !follow_final) {
                *inode_out = current_ino;
                return 0;
            }

            /* Follow the symlink */
            if (++symlink_depth > SYMLINK_MAX_DEPTH)
                return -ELOOP;

            char target[256];
            int tlen = ext2_read_symlink_target(current_ino, target, 255);
            if (tlen <= 0)
                return -EIO;
            target[tlen] = '\0';

            /* Build new path: target + "/" + remaining */
            char newpath[512];
            uint32_t ni = 0;
            uint32_t ti;
            for (ti = 0; ti < (uint32_t)tlen && ni < 510; ti++)
                newpath[ni++] = target[ti];
            if (*p != '\0') {
                if (ni < 510)
                    newpath[ni++] = '/';
                while (*p != '\0' && ni < 510)
                    newpath[ni++] = *p++;
            }
            newpath[ni] = '\0';

            /* Copy newpath back into resolved */
            for (ri = 0; ri < 511 && newpath[ri]; ri++)
                resolved[ri] = newpath[ri];
            resolved[ri] = '\0';
            p = resolved;

            /* Absolute symlink → restart from root */
            if (resolved[0] == '/') {
                current_ino = EXT2_ROOT_INODE;
                while (*p == '/')
                    p++;
            } else {
                /* Relative symlink → restart from parent directory */
                current_ino = parent_ino;
            }
            goto restart_walk;
        }
    }

    *inode_out = current_ino;
    return 0;
}

int ext2_open(const char *path, uint32_t *inode_out)
{
    return ext2_open_ex(path, inode_out, 1);  /* follow symlinks by default */
}
```

- [ ] **Step 7: Implement `ext2_symlink`**

Add after `ext2_open`:

```c
/* ------------------------------------------------------------------ */
/* ext2_symlink — create a symbolic link                               */
/* ------------------------------------------------------------------ */

int ext2_symlink(const char *linkpath, const char *target)
{
    if (!s_mounted)
        return -1;
    irqflags_t fl = spin_lock_irqsave(&ext2_lock);

    uint32_t parent_ino;
    const char *basename;
    if (ext2_lookup_parent(linkpath, &parent_ino, &basename) != 0) {
        spin_unlock_irqrestore(&ext2_lock, fl);
        return -1;
    }

    uint32_t new_ino = ext2_alloc_inode(0);
    if (new_ino == 0) {
        spin_unlock_irqrestore(&ext2_lock, fl);
        return -1;
    }

    /* Compute target length */
    uint32_t tlen = 0;
    while (target[tlen] != '\0')
        tlen++;

    ext2_inode_t inode;
    uint32_t ci;
    for (ci = 0; ci < sizeof(inode); ci++)
        ((uint8_t *)&inode)[ci] = 0;
    inode.i_mode = EXT2_S_IFLNK | 0777;
    inode.i_size = tlen;
    inode.i_links_count = 1;

    if (tlen <= 60) {
        /* Fast symlink: store target in i_block[] */
        uint8_t *dst = (uint8_t *)inode.i_block;
        for (ci = 0; ci < tlen; ci++)
            dst[ci] = (uint8_t)target[ci];
        inode.i_blocks = 0;
    } else {
        /* Slow symlink: allocate a data block */
        uint32_t blk = ext2_alloc_block(0);
        if (blk == 0) {
            spin_unlock_irqrestore(&ext2_lock, fl);
            return -1;
        }
        uint8_t *data = cache_get_slot(blk);
        if (!data) {
            spin_unlock_irqrestore(&ext2_lock, fl);
            return -1;
        }
        for (ci = 0; ci < tlen; ci++)
            data[ci] = (uint8_t)target[ci];
        for (; ci < s_block_size; ci++)
            data[ci] = 0;
        cache_mark_dirty(blk);
        inode.i_block[0] = blk;
        inode.i_blocks = s_block_size / 512;
    }

    ext2_write_inode(new_ino, &inode);
    int r = ext2_dir_add_entry(parent_ino, new_ino, basename, EXT2_FT_SYMLINK);
    spin_unlock_irqrestore(&ext2_lock, fl);
    return r;
}
```

- [ ] **Step 8: Implement `ext2_readlink`**

```c
/* ------------------------------------------------------------------ */
/* ext2_readlink — read symlink target by path                         */
/* ------------------------------------------------------------------ */

int ext2_readlink(const char *path, char *buf, uint32_t bufsiz)
{
    if (!s_mounted)
        return -1;
    irqflags_t fl = spin_lock_irqsave(&ext2_lock);

    uint32_t ino;
    if (ext2_open_ex(path, &ino, 0) != 0) {  /* no-follow on final component */
        spin_unlock_irqrestore(&ext2_lock, fl);
        return -1;
    }

    int r = ext2_read_symlink_target(ino, buf, bufsiz);
    spin_unlock_irqrestore(&ext2_lock, fl);
    return r;
}
```

- [ ] **Step 9: Implement `ext2_chmod` and `ext2_chown`**

```c
/* ------------------------------------------------------------------ */
/* ext2_chmod — change file mode bits                                  */
/* ------------------------------------------------------------------ */

int ext2_chmod(const char *path, uint16_t mode)
{
    if (!s_mounted)
        return -1;
    irqflags_t fl = spin_lock_irqsave(&ext2_lock);

    uint32_t ino;
    if (ext2_open_ex(path, &ino, 1) != 0) {  /* follow symlinks */
        spin_unlock_irqrestore(&ext2_lock, fl);
        return -1;
    }

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0) {
        spin_unlock_irqrestore(&ext2_lock, fl);
        return -EIO;
    }

    inode.i_mode = (inode.i_mode & (uint16_t)EXT2_S_IFMT) | (mode & 0x1FFu);
    ext2_write_inode(ino, &inode);
    spin_unlock_irqrestore(&ext2_lock, fl);
    return 0;
}

/* ------------------------------------------------------------------ */
/* ext2_chown — change file owner/group                                */
/* ------------------------------------------------------------------ */

int ext2_chown(const char *path, uint16_t uid, uint16_t gid, int follow)
{
    if (!s_mounted)
        return -1;
    irqflags_t fl = spin_lock_irqsave(&ext2_lock);

    uint32_t ino;
    if (ext2_open_ex(path, &ino, follow) != 0) {
        spin_unlock_irqrestore(&ext2_lock, fl);
        return -1;
    }

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0) {
        spin_unlock_irqrestore(&ext2_lock, fl);
        return -EIO;
    }

    inode.i_uid = uid;
    inode.i_gid = gid;
    ext2_write_inode(ino, &inode);
    spin_unlock_irqrestore(&ext2_lock, fl);
    return 0;
}
```

- [ ] **Step 10: Verify build compiles**

Run: `make -C /Users/dylan/Developer/aegis clean && make -C /Users/dylan/Developer/aegis 2>&1 | tail -20`
Expected: Clean build, no errors.

- [ ] **Step 11: Commit**

```bash
git add kernel/fs/ext2.h kernel/fs/ext2.c kernel/fs/ext2_internal.h kernel/fs/vfs.h
git commit -m "ext2: add symlink creation/reading/following + chmod/chown + permission check"
```

---

### Task 2: Wire syscalls + VFS stat changes

**Files:**
- Modify: `kernel/syscall/syscall.c` (add cases 88-94, fix case 6)
- Modify: `kernel/syscall/sys_file.c` (add sys_symlink, sys_readlink, sys_chmod, sys_fchmod, sys_chown, sys_fchown, sys_lchown, sys_lstat, fix sys_access)
- Modify: `kernel/syscall/sys_impl.h` (add new function declarations)
- Modify: `kernel/fs/vfs.c` (add `vfs_stat_path_ex` with follow flag)
- Modify: `kernel/fs/vfs.h` (add `vfs_stat_path_ex` prototype)

- [ ] **Step 1: Add `vfs_stat_path_ex` to vfs.h**

Add after line 125 (`int vfs_stat_path(...)`):

```c
/* vfs_stat_path_ex — stat with follow_symlink control.
 * follow=1 for stat (follow symlinks), follow=0 for lstat. */
int vfs_stat_path_ex(const char *path, k_stat_t *out, int follow);
```

- [ ] **Step 2: Implement `vfs_stat_path_ex` in vfs.c**

The existing `vfs_stat_path` calls `ext2_open(path, &ino)` which now follows symlinks. For lstat, we need `ext2_open_ex(path, &ino, 0)`. Change the ext2 block in `vfs_stat_path` (lines 407-429) and add the new function:

After line 436 (`return -2;` at end of `vfs_stat_path`), add:

```c
int
vfs_stat_path_ex(const char *path, k_stat_t *out, int follow)
{
    if (!path || !out) return -2;

    /* Non-ext2 paths: same as vfs_stat_path (no symlinks on those) */
    /* /proc → procfs stat */
    if (path[0]=='/' && path[1]=='p' && path[2]=='r' && path[3]=='o' &&
        path[4]=='c' && (path[5]=='/' || path[5]=='\0'))
        return procfs_stat(path, out);

    /* /dev/ device specials — delegate to existing vfs_stat_path */
    if (path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v')
        return vfs_stat_path(path, out);

    /* Synthetic dirs */
    if (streq(path, "/proc") || streq(path, "/tmp") || streq(path, "/run"))
        return vfs_stat_path(path, out);

    /* /tmp/ -> tmp ramfs stat */
    if (path[0]=='/' && path[1]=='t' && path[2]=='m' && path[3]=='p' && path[4]=='/')
        return ramfs_stat(&s_tmp_ramfs, path + 5, out);

    /* /run/ -> run ramfs stat */
    if (path[0]=='/' && path[1]=='r' && path[2]=='u' && path[3]=='n' && path[4]=='/')
        return ramfs_stat(&s_run_ramfs, path + 5, out);

    /* ext2 with follow control */
    {
        uint32_t ino = 0;
        if (ext2_open_ex(path, &ino, follow) == 0) {
            int sz = ext2_file_size(ino);
            if (sz < 0) sz = 0;
            ext2_inode_t inode;
            uint32_t mode;
            if (ext2_read_inode(ino, &inode) == 0)
                mode = (uint32_t)inode.i_mode;
            else
                mode = S_IFREG | 0644;
            __builtin_memset(out, 0, sizeof(*out));
            out->st_dev     = 2;
            out->st_ino     = (uint64_t)ino;
            out->st_nlink   = 1;
            out->st_mode    = mode;
            out->st_uid     = (uint32_t)inode.i_uid;
            out->st_gid     = (uint32_t)inode.i_gid;
            out->st_size    = (int64_t)sz;
            out->st_blksize = 4096;
            out->st_blocks  = (int64_t)(((uint64_t)sz + 511) / 512 * 8);
            return 0;
        }
    }

    /* initrd fallback */
    if (initrd_stat_entry(path, out) == 0)
        return 0;

    return -2;
}
```

- [ ] **Step 3: Add new syscall declarations to sys_impl.h**

Find the file and add declarations for the new syscalls:

```c
/* Phase 41: symlinks + chmod/chown */
uint64_t sys_lstat(uint64_t arg1, uint64_t arg2);
uint64_t sys_symlink(uint64_t arg1, uint64_t arg2);
uint64_t sys_readlink(uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_chmod(uint64_t arg1, uint64_t arg2);
uint64_t sys_fchmod(uint64_t arg1, uint64_t arg2);
uint64_t sys_chown(uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_fchown(uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_lchown(uint64_t arg1, uint64_t arg2, uint64_t arg3);
```

- [ ] **Step 4: Wire syscall numbers in syscall.c**

Change line 98 from:
```c
    case  6: return sys_stat(arg1, arg2);   /* lstat = stat (no symlinks) */
```
to:
```c
    case  6: return sys_lstat(arg1, arg2);
```

Add new cases near the other file syscalls (after case 87 / sys_unlink):

```c
    case 88: return sys_symlink(arg1, arg2);
    case 89: return sys_readlink(arg1, arg2, arg3);
    case 90: return sys_chmod(arg1, arg2);
    case 91: return sys_fchmod(arg1, arg2);
    case 92: return sys_chown(arg1, arg2, arg3);
    case 93: return sys_fchown(arg1, arg2, arg3);
    case 94: return sys_lchown(arg1, arg2, arg3);
```

Also add ARM64 translations in the aarch64 block:

```c
    case  36: num = 88; arg1 = arg2; arg2 = arg3; break;  /* symlinkat → symlink (skip dirfd) */
    case  78: num = 89; arg1 = arg2; arg2 = arg3; arg3 = arg4; break;  /* readlinkat → readlink (skip dirfd) */
    case  53: num = 90;  break;  /* fchmodat → chmod (approx) */
    case  54: num = 93;  break;  /* fchownat → fchown (approx) */
```

- [ ] **Step 5: Implement sys_lstat in sys_file.c**

```c
uint64_t
sys_lstat(uint64_t arg1, uint64_t arg2)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current()->priv;
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_READ, CAP_RIGHTS_READ) != 0)
        return (uint64_t)(-ENOCAP);

    char kpath[256];
    uint32_t i;
    for (i = 0; i < 255; i++) {
        uint8_t ch;
        copy_from_user(&ch, (const void *)(uintptr_t)(arg1 + i), 1);
        kpath[i] = (char)ch;
        if (ch == 0) break;
    }
    kpath[255] = '\0';

    /* Resolve relative paths */
    char full[512];
    if (kpath[0] != '/' && proc->cwd[0] != '\0') {
        uint32_t ci = 0;
        const char *cw = proc->cwd;
        while (*cw && ci < 510) full[ci++] = *cw++;
        if (ci > 0 && full[ci-1] != '/' && ci < 510) full[ci++] = '/';
        uint32_t ki = 0;
        while (kpath[ki] && ci < 510) full[ci++] = kpath[ki++];
        full[ci] = '\0';
    } else {
        uint32_t ci = 0;
        while (kpath[ci] && ci < 510) { full[ci] = kpath[ci]; ci++; }
        full[ci] = '\0';
    }

    k_stat_t ks;
    if (vfs_stat_path_ex(full, &ks, 0) != 0)  /* follow=0 for lstat */
        return (uint64_t)(-2);  /* ENOENT */

    copy_to_user((void *)(uintptr_t)arg2, &ks, sizeof(ks));
    return 0;
}
```

- [ ] **Step 6: Implement sys_symlink in sys_file.c**

```c
uint64_t
sys_symlink(uint64_t arg1, uint64_t arg2)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current()->priv;
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_WRITE, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)(-ENOCAP);

    /* arg1 = target, arg2 = linkpath */
    char target[256], linkpath[256];
    uint32_t i;
    for (i = 0; i < 255; i++) {
        uint8_t ch;
        copy_from_user(&ch, (const void *)(uintptr_t)(arg1 + i), 1);
        target[i] = (char)ch;
        if (ch == 0) break;
    }
    target[255] = '\0';

    for (i = 0; i < 255; i++) {
        uint8_t ch;
        copy_from_user(&ch, (const void *)(uintptr_t)(arg2 + i), 1);
        linkpath[i] = (char)ch;
        if (ch == 0) break;
    }
    linkpath[255] = '\0';

    /* Resolve relative linkpath against cwd */
    char full[512];
    if (linkpath[0] != '/' && proc->cwd[0] != '\0') {
        uint32_t ci = 0;
        const char *cw = proc->cwd;
        while (*cw && ci < 510) full[ci++] = *cw++;
        if (ci > 0 && full[ci-1] != '/' && ci < 510) full[ci++] = '/';
        uint32_t ki = 0;
        while (linkpath[ki] && ci < 510) full[ci++] = linkpath[ki++];
        full[ci] = '\0';
    } else {
        uint32_t ci = 0;
        while (linkpath[ci] && ci < 510) { full[ci] = linkpath[ci]; ci++; }
        full[ci] = '\0';
    }

    int r = ext2_symlink(full, target);
    return (r == 0) ? 0 : (uint64_t)(-EIO);
}
```

- [ ] **Step 7: Implement sys_readlink in sys_file.c**

```c
uint64_t
sys_readlink(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current()->priv;
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_READ, CAP_RIGHTS_READ) != 0)
        return (uint64_t)(-ENOCAP);

    char kpath[256];
    uint32_t i;
    for (i = 0; i < 255; i++) {
        uint8_t ch;
        copy_from_user(&ch, (const void *)(uintptr_t)(arg1 + i), 1);
        kpath[i] = (char)ch;
        if (ch == 0) break;
    }
    kpath[255] = '\0';

    /* Resolve relative path */
    char full[512];
    if (kpath[0] != '/' && proc->cwd[0] != '\0') {
        uint32_t ci = 0;
        const char *cw = proc->cwd;
        while (*cw && ci < 510) full[ci++] = *cw++;
        if (ci > 0 && full[ci-1] != '/' && ci < 510) full[ci++] = '/';
        uint32_t ki = 0;
        while (kpath[ki] && ci < 510) full[ci++] = kpath[ki++];
        full[ci] = '\0';
    } else {
        uint32_t ci = 0;
        while (kpath[ci] && ci < 510) { full[ci] = kpath[ci]; ci++; }
        full[ci] = '\0';
    }

    uint32_t bufsiz = (uint32_t)arg3;
    if (bufsiz > 4096) bufsiz = 4096;

    char kbuf[256];
    if (bufsiz > 256) bufsiz = 256;
    int r = ext2_readlink(full, kbuf, bufsiz);
    if (r < 0)
        return (uint64_t)(int64_t)r;

    copy_to_user((void *)(uintptr_t)arg2, kbuf, (uint32_t)r);
    return (uint64_t)r;
}
```

- [ ] **Step 8: Implement sys_chmod, sys_fchmod, sys_chown, sys_fchown, sys_lchown in sys_file.c**

```c
uint64_t
sys_chmod(uint64_t arg1, uint64_t arg2)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current()->priv;
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_WRITE, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)(-ENOCAP);

    char kpath[256];
    uint32_t i;
    for (i = 0; i < 255; i++) {
        uint8_t ch;
        copy_from_user(&ch, (const void *)(uintptr_t)(arg1 + i), 1);
        kpath[i] = (char)ch;
        if (ch == 0) break;
    }
    kpath[255] = '\0';

    char full[512];
    if (kpath[0] != '/' && proc->cwd[0] != '\0') {
        uint32_t ci = 0;
        const char *cw = proc->cwd;
        while (*cw && ci < 510) full[ci++] = *cw++;
        if (ci > 0 && full[ci-1] != '/' && ci < 510) full[ci++] = '/';
        uint32_t ki = 0;
        while (kpath[ki] && ci < 510) full[ci++] = kpath[ki++];
        full[ci] = '\0';
    } else {
        uint32_t ci = 0;
        while (kpath[ci] && ci < 510) { full[ci] = kpath[ci]; ci++; }
        full[ci] = '\0';
    }

    int r = ext2_chmod(full, (uint16_t)(arg2 & 0x1FFu));
    return (r == 0) ? 0 : (uint64_t)(int64_t)r;
}

uint64_t
sys_fchmod(uint64_t arg1, uint64_t arg2)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current()->priv;
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_WRITE, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)(-ENOCAP);

    int fd = (int)arg1;
    if (fd < 0 || fd >= PROC_MAX_FDS)
        return (uint64_t)(-9);  /* EBADF */

    vfs_file_t *f = &proc->fd_table->fds[fd];
    if (!f->ops)
        return (uint64_t)(-9);  /* EBADF */

    /* Only ext2 files have inode numbers we can modify */
    if (f->ops != &s_ext2_ops)
        return (uint64_t)(-1);  /* EPERM — can't chmod non-ext2 */

    ext2_fd_priv_t *p = (ext2_fd_priv_t *)f->priv;
    ext2_inode_t inode;
    if (ext2_read_inode(p->ino, &inode) != 0)
        return (uint64_t)(-EIO);

    inode.i_mode = (inode.i_mode & (uint16_t)EXT2_S_IFMT) | (uint16_t)(arg2 & 0x1FFu);
    ext2_write_inode(p->ino, &inode);
    return 0;
}

uint64_t
sys_chown(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current()->priv;
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_SETUID, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)(-ENOCAP);

    char kpath[256];
    uint32_t i;
    for (i = 0; i < 255; i++) {
        uint8_t ch;
        copy_from_user(&ch, (const void *)(uintptr_t)(arg1 + i), 1);
        kpath[i] = (char)ch;
        if (ch == 0) break;
    }
    kpath[255] = '\0';

    char full[512];
    if (kpath[0] != '/' && proc->cwd[0] != '\0') {
        uint32_t ci = 0;
        const char *cw = proc->cwd;
        while (*cw && ci < 510) full[ci++] = *cw++;
        if (ci > 0 && full[ci-1] != '/' && ci < 510) full[ci++] = '/';
        uint32_t ki = 0;
        while (kpath[ki] && ci < 510) full[ci++] = kpath[ki++];
        full[ci] = '\0';
    } else {
        uint32_t ci = 0;
        while (kpath[ci] && ci < 510) { full[ci] = kpath[ci]; ci++; }
        full[ci] = '\0';
    }

    int r = ext2_chown(full, (uint16_t)arg2, (uint16_t)arg3, 1);  /* follow */
    return (r == 0) ? 0 : (uint64_t)(int64_t)r;
}

uint64_t
sys_fchown(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current()->priv;
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_SETUID, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)(-ENOCAP);

    int fd = (int)arg1;
    if (fd < 0 || fd >= PROC_MAX_FDS)
        return (uint64_t)(-9);

    vfs_file_t *f = &proc->fd_table->fds[fd];
    if (!f->ops)
        return (uint64_t)(-9);

    if (f->ops != &s_ext2_ops)
        return (uint64_t)(-1);

    ext2_fd_priv_t *p = (ext2_fd_priv_t *)f->priv;
    ext2_inode_t inode;
    if (ext2_read_inode(p->ino, &inode) != 0)
        return (uint64_t)(-EIO);

    inode.i_uid = (uint16_t)arg2;
    inode.i_gid = (uint16_t)arg3;
    ext2_write_inode(p->ino, &inode);
    return 0;
}

uint64_t
sys_lchown(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current()->priv;
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_SETUID, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)(-ENOCAP);

    char kpath[256];
    uint32_t i;
    for (i = 0; i < 255; i++) {
        uint8_t ch;
        copy_from_user(&ch, (const void *)(uintptr_t)(arg1 + i), 1);
        kpath[i] = (char)ch;
        if (ch == 0) break;
    }
    kpath[255] = '\0';

    char full[512];
    if (kpath[0] != '/' && proc->cwd[0] != '\0') {
        uint32_t ci = 0;
        const char *cw = proc->cwd;
        while (*cw && ci < 510) full[ci++] = *cw++;
        if (ci > 0 && full[ci-1] != '/' && ci < 510) full[ci++] = '/';
        uint32_t ki = 0;
        while (kpath[ki] && ci < 510) full[ci++] = kpath[ki++];
        full[ci] = '\0';
    } else {
        uint32_t ci = 0;
        while (kpath[ci] && ci < 510) { full[ci] = kpath[ci]; ci++; }
        full[ci] = '\0';
    }

    int r = ext2_chown(full, (uint16_t)arg2, (uint16_t)arg3, 0);  /* no follow */
    return (r == 0) ? 0 : (uint64_t)(int64_t)r;
}
```

- [ ] **Step 9: Fix sys_access to check mode bits**

Replace the existing `sys_access` (around line 326-335) with one that actually checks permissions:

```c
uint64_t
sys_access(uint64_t arg1, uint64_t arg2)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current()->priv;

    char kpath[256];
    uint32_t i;
    for (i = 0; i < 255; i++) {
        uint8_t ch;
        copy_from_user(&ch, (const void *)(uintptr_t)(arg1 + i), 1);
        kpath[i] = (char)ch;
        if (ch == 0) break;
    }
    kpath[255] = '\0';

    char full[512];
    if (kpath[0] != '/' && proc->cwd[0] != '\0') {
        uint32_t ci = 0;
        const char *cw = proc->cwd;
        while (*cw && ci < 510) full[ci++] = *cw++;
        if (ci > 0 && full[ci-1] != '/' && ci < 510) full[ci++] = '/';
        uint32_t ki = 0;
        while (kpath[ki] && ci < 510) full[ci++] = kpath[ki++];
        full[ci] = '\0';
    } else {
        uint32_t ci = 0;
        while (kpath[ci] && ci < 510) { full[ci] = kpath[ci]; ci++; }
        full[ci] = '\0';
    }

    /* F_OK (0) = existence check only */
    int mode = (int)arg2;

    /* Check existence first */
    uint32_t ino;
    if (ext2_open(full, &ino) == 0) {
        if (mode == 0)
            return 0;  /* F_OK — exists */
        /* Check permission bits */
        int want = 0;
        if (mode & 4) want |= 4;  /* R_OK */
        if (mode & 2) want |= 2;  /* W_OK */
        if (mode & 1) want |= 1;  /* X_OK */
        return (uint64_t)(int64_t)ext2_check_perm(ino,
            (uint16_t)proc->uid, (uint16_t)proc->gid, want);
    }

    /* Fallback: non-ext2 files — existence check via vfs_stat_path */
    k_stat_t ks;
    if (vfs_stat_path(full, &ks) == 0)
        return 0;

    return (uint64_t)(-2);  /* ENOENT */
}
```

- [ ] **Step 10: Verify build compiles**

Run: `make -C /Users/dylan/Developer/aegis clean && make -C /Users/dylan/Developer/aegis 2>&1 | tail -20`
Expected: Clean build, no errors.

- [ ] **Step 11: Commit**

```bash
git add kernel/syscall/syscall.c kernel/syscall/sys_file.c kernel/syscall/sys_impl.h kernel/fs/vfs.c kernel/fs/vfs.h
git commit -m "syscall: wire symlink/readlink/chmod/fchmod/chown/fchown/lchown + fix lstat and access"
```

---

### Task 3: Permission enforcement in sys_open, sys_execve, sys_unlink, sys_mkdir, sys_rename

**Files:**
- Modify: `kernel/syscall/sys_file.c` (add DAC checks to sys_open, sys_mkdir, sys_unlink, sys_rename)
- Modify: `kernel/syscall/sys_process.c` (add X_OK check to sys_execve)
- Modify: `kernel/fs/vfs.c` (add permission check in vfs_open for ext2 files)

- [ ] **Step 1: Add permission check to vfs_open for ext2 files**

In `vfs_open`, after the ext2_open succeeds (around line 246-258), add a permission check before returning the fd. Modify the ext2 block:

```c
    /* ext2 primary — writable root filesystem */
    {
        uint32_t ino = 0;
        if (ext2_open(path, &ino) >= 0) {
            /* DAC permission check */
            aegis_process_t *proc = (aegis_process_t *)sched_current()->priv;
            int want = 4;  /* R_OK by default */
            if (flags & 1) want = 2;       /* O_WRONLY */
            if (flags & 2) want = 4 | 2;   /* O_RDWR */
            int perm = ext2_check_perm(ino,
                (uint16_t)proc->uid, (uint16_t)proc->gid, want);
            if (perm != 0)
                return -EACCES;

            ext2_fd_priv_t *p = ext2_pool_alloc(ino);
            if (!p) return -12;
            int sz = ext2_file_size(ino);
            if (sz < 0) sz = 0;
            out->ops    = &s_ext2_ops;
            out->priv   = (void *)p;
            out->offset = 0;
            out->size   = (uint64_t)sz;
            out->flags  = 0;
            out->_pad   = 0;
            return 0;
        }
        /* ext2 ENOENT + O_CREAT → check W_OK on parent, then create */
        if (flags & (int)VFS_O_CREAT) {
            /* Check write permission on parent directory */
            uint32_t parent_ino;
            const char *bname;
            if (ext2_lookup_parent(path, &parent_ino, &bname) == 0) {
                aegis_process_t *proc = (aegis_process_t *)sched_current()->priv;
                int pperm = ext2_check_perm(parent_ino,
                    (uint16_t)proc->uid, (uint16_t)proc->gid, 2 | 1); /* W+X */
                if (pperm != 0)
                    return -EACCES;
            }
            if (ext2_create(path, 0644) == 0) {
                if (ext2_open(path, &ino) >= 0) {
                    ext2_fd_priv_t *p = ext2_pool_alloc(ino);
                    if (!p) return -12;
                    out->ops    = &s_ext2_ops;
                    out->priv   = (void *)p;
                    out->offset = 0;
                    out->size   = 0;
                    out->flags  = 0;
                    out->_pad   = 0;
                    return 0;
                }
            }
        }
    }
```

Note: `vfs_open` needs access to `sched_current` and the process struct. Add includes at the top of vfs.c:

```c
#include "../sched/sched.h"
#include "../proc/proc.h"
#include "../cap/cap.h"
```

- [ ] **Step 2: Add W_OK+X_OK check on parent directory for sys_unlink, sys_mkdir, sys_rename**

In `sys_unlink` (around line 767-779), after copying the path and before calling `ext2_unlink`:

```c
    /* Check W+X permission on parent directory */
    {
        uint32_t parent_ino;
        const char *bname;
        if (ext2_lookup_parent(kpath, &parent_ino, &bname) == 0) {
            int pperm = ext2_check_perm(parent_ino,
                (uint16_t)proc->uid, (uint16_t)proc->gid, 2 | 1);
            if (pperm != 0)
                return (uint64_t)(-EACCES);
        }
    }
```

Add the same pattern in `sys_mkdir` and `sys_rename` (for both source and destination parent).

- [ ] **Step 3: Add X_OK check to sys_execve**

In `sys_process.c`, in the `sys_execve` function, after the ELF path is resolved but before loading, add:

```c
    /* Check execute permission on the ELF binary */
    {
        uint32_t elf_ino;
        if (ext2_open(kpath, &elf_ino) == 0) {
            int xperm = ext2_check_perm(elf_ino,
                (uint16_t)proc->uid, (uint16_t)proc->gid, 1); /* X_OK */
            if (xperm != 0)
                return -EACCES;
        }
    }
```

- [ ] **Step 4: Verify build and make test**

Run: `make -C /Users/dylan/Developer/aegis clean && make -C /Users/dylan/Developer/aegis test 2>&1 | tail -30`
Expected: `make test` passes (existing tests must not break — all files are currently owned by uid=0 with modes 0644/0755, and processes run as uid=0, so owner bits allow everything).

- [ ] **Step 5: Commit**

```bash
git add kernel/syscall/sys_file.c kernel/syscall/sys_process.c kernel/fs/vfs.c
git commit -m "enforce: DAC permission checks in sys_open/access/unlink/mkdir/rename/execve"
```

---

### Task 4: ext2 readdir symlink type + vfs_stat uid/gid population

**Files:**
- Modify: `kernel/fs/ext2.c` (readdir returns symlink type)
- Modify: `kernel/fs/vfs.c` (vfs_stat_path populates st_uid/st_gid, ext2_vfs_stat_fn populates uid/gid)

- [ ] **Step 1: Update ext2_readdir to return symlink type**

In `ext2_readdir` (ext2.c around line 461), change the type_out assignment:

```c
                    if (de->file_type == EXT2_FT_DIR)
                        *type_out = 4u;   /* DT_DIR */
                    else if (de->file_type == EXT2_FT_SYMLINK)
                        *type_out = 10u;  /* DT_LNK */
                    else
                        *type_out = 8u;   /* DT_REG */
```

- [ ] **Step 2: Populate st_uid/st_gid in ext2_vfs_stat_fn**

In `ext2_vfs_stat_fn` (vfs.c around line 151-167), add uid/gid after st_mode:

```c
    st->st_uid     = (uint32_t)inode.i_uid;
    st->st_gid     = (uint32_t)inode.i_gid;
```

- [ ] **Step 3: Populate st_uid/st_gid in vfs_stat_path ext2 block**

In `vfs_stat_path` (vfs.c around line 407-429), add uid/gid from the inode that's already read:

```c
            out->st_uid     = (uint32_t)inode.i_uid;
            out->st_gid     = (uint32_t)inode.i_gid;
```

- [ ] **Step 4: Verify build**

Run: `make -C /Users/dylan/Developer/aegis 2>&1 | tail -10`
Expected: Clean build.

- [ ] **Step 5: Commit**

```bash
git add kernel/fs/ext2.c kernel/fs/vfs.c
git commit -m "ext2: readdir reports DT_LNK for symlinks + stat populates uid/gid"
```

---

### Task 5: User-space tools (`ln`, `chmod`, `chown`, `readlink`)

**Files:**
- Create: `user/ln/ln.c` + `user/ln/Makefile`
- Create: `user/chmod/chmod.c` + `user/chmod/Makefile`
- Create: `user/chown/chown.c` + `user/chown/Makefile`
- Create: `user/readlink/readlink.c` + `user/readlink/Makefile`
- Modify: `Makefile` (add to DISK_BINS for ext2 disk image)

- [ ] **Step 1: Create user/ln/ln.c**

```c
#include <stdio.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "-s") == 0)
        return symlink(argv[2], argv[3]) == 0 ? 0 : 1;
    if (argc == 3)
        return symlink(argv[1], argv[2]) == 0 ? 0 : 1;
    write(2, "usage: ln [-s] target linkname\n", 30);
    return 1;
}
```

- [ ] **Step 2: Create user/ln/Makefile**

Follow the pattern of other user Makefiles (e.g. user/cp/Makefile):

```makefile
include ../common.mk
```

(If common.mk doesn't exist, use the same pattern as user/rm/ or user/touch/.)

- [ ] **Step 3: Create user/chmod/chmod.c**

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        write(2, "usage: chmod MODE FILE\n", 23);
        return 1;
    }
    unsigned mode = strtoul(argv[1], NULL, 8);
    return chmod(argv[2], mode) == 0 ? 0 : 1;
}
```

- [ ] **Step 4: Create user/chmod/Makefile**

```makefile
include ../common.mk
```

- [ ] **Step 5: Create user/chown/chown.c**

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        write(2, "usage: chown UID:GID FILE\n", 26);
        return 1;
    }
    /* Parse uid:gid */
    char *colon = strchr(argv[1], ':');
    if (!colon) {
        write(2, "format: UID:GID\n", 16);
        return 1;
    }
    *colon = '\0';
    uid_t uid = (uid_t)atoi(argv[1]);
    gid_t gid = (gid_t)atoi(colon + 1);
    return chown(argv[2], uid, gid) == 0 ? 0 : 1;
}
```

- [ ] **Step 6: Create user/chown/Makefile**

```makefile
include ../common.mk
```

- [ ] **Step 7: Create user/readlink/readlink.c**

```c
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        write(2, "usage: readlink PATH\n", 21);
        return 1;
    }
    char buf[256];
    ssize_t n = readlink(argv[1], buf, sizeof(buf) - 1);
    if (n < 0) return 1;
    buf[n] = '\n';
    write(1, buf, n + 1);
    return 0;
}
```

- [ ] **Step 8: Create user/readlink/Makefile**

```makefile
include ../common.mk
```

- [ ] **Step 9: Add tools to disk build**

Add `ln`, `chmod`, `chown`, `readlink` to the `DISK_BINS` list in the root Makefile so they're included in the ext2 disk image.

- [ ] **Step 10: Verify build**

Run: `make -C /Users/dylan/Developer/aegis disk 2>&1 | tail -20`
Expected: Disk image builds with new tools included.

- [ ] **Step 11: Commit**

```bash
git add user/ln/ user/chmod/ user/chown/ user/readlink/ Makefile
git commit -m "user: add ln, chmod, chown, readlink tools for symlink/permission testing"
```

---

### Task 6: Test script + `make test` verification

**Files:**
- Create: `tests/test_symlink.py`
- Modify: `tests/run_tests.sh` (add test_symlink.py)

- [ ] **Step 1: Create tests/test_symlink.py**

```python
#!/usr/bin/env python3
"""test_symlink.py — Phase 41 symlink + chmod smoke test.

Boots Aegis with q35 + NVMe disk, logs in, tests:
1. symlink creation (ln -s)
2. readlink
3. read through symlink (cat)
4. lstat vs stat (S_IFLNK vs S_IFREG)
5. symlink chain resolution
6. ELOOP on circular symlinks
7. chmod + permission enforcement
"""
import subprocess, sys, os, select, fcntl, time, socket, tempfile

QEMU = "qemu-system-x86_64"
ISO  = "build/aegis.iso"
DISK = "build/disk.img"
BOOT_TIMEOUT = 120
CMD_TIMEOUT  = 30

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

_KEY_MAP = {
    ' ': 'spc', '\n': 'ret', '/': 'slash', '-': 'minus', '.': 'dot',
    ':': 'shift-semicolon', '|': 'shift-backslash', '>': 'shift-dot',
    '<': 'shift-comma', '_': 'shift-minus', '=': 'equal',
    '~': 'shift-grave_accent',
}
for c in 'abcdefghijklmnopqrstuvwxyz': _KEY_MAP[c] = c
for c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ': _KEY_MAP[c] = f'shift-{c.lower()}'
for c in '0123456789': _KEY_MAP.setdefault(c, c)


def build_iso():
    r = subprocess.run("make INIT=vigil iso", shell=True,
                       cwd=ROOT, capture_output=True)
    if r.returncode != 0:
        print("[FAIL] make INIT=vigil iso failed")
        print(r.stderr.decode())
        sys.exit(1)


def _type_string(mon_sock, s):
    for ch in s:
        key = _KEY_MAP.get(ch)
        if key is None:
            continue
        mon_sock.sendall(f'sendkey {key}\n'.encode())
        time.sleep(0.08)
        try:
            mon_sock.recv(4096)
        except OSError:
            pass


class SerialReader:
    def __init__(self, fd):
        self._fd = fd
        self._buf = b""

    def _drain(self, timeout=0.5):
        ready, _, _ = select.select([self._fd], [], [], timeout)
        if ready:
            try:
                chunk = os.read(self._fd, 65536)
                if chunk:
                    self._buf += chunk
                    sys.stdout.write(chunk.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
            except (BlockingIOError, OSError):
                pass

    def wait_for(self, needle, deadline):
        enc = needle.encode() if isinstance(needle, str) else needle
        while time.time() < deadline:
            if enc in self._buf:
                return True
            self._drain()
        return enc in self._buf

    def full_output(self):
        return self._buf.decode("utf-8", errors="replace")

    def clear(self):
        self._buf = b""


def run_test():
    iso_path  = os.path.join(ROOT, ISO)
    disk_path = os.path.join(ROOT, DISK)

    if not os.path.exists(disk_path):
        print(f"SKIP: {DISK} not found — run 'make disk' first")
        sys.exit(0)

    build_iso()

    mon_path = tempfile.mktemp(suffix=".sock")
    proc = subprocess.Popen(
        [QEMU,
         "-machine", "q35", "-cpu", "Broadwell",
         "-cdrom", iso_path, "-boot", "order=d",
         "-display", "none", "-vga", "std", "-nodefaults",
         "-serial", "stdio", "-no-reboot", "-m", "2G",
         "-drive", f"file={disk_path},if=none,id=nvme0,format=raw",
         "-device", "nvme,drive=nvme0,serial=aegis0",
         "-device", "virtio-net-pci,netdev=n0,disable-legacy=on",
         "-netdev", "user,id=n0",
         "-monitor", f"unix:{mon_path},server,nowait"],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    flags = fcntl.fcntl(proc.stdout.fileno(), fcntl.F_GETFL)
    fcntl.fcntl(proc.stdout.fileno(), fcntl.F_SETFL, flags | os.O_NONBLOCK)
    serial = SerialReader(proc.stdout.fileno())

    deadline = time.time() + 10
    while not os.path.exists(mon_path) and time.time() < deadline:
        time.sleep(0.1)
    mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    mon.connect(mon_path)
    mon.setblocking(False)

    passed = 0
    failed = 0

    def check(name, needle, timeout=CMD_TIMEOUT):
        nonlocal passed, failed
        if serial.wait_for(needle, time.time() + timeout):
            print(f"  PASS: {name}")
            passed += 1
        else:
            print(f"  FAIL: {name} — '{needle}' not found")
            failed += 1

    try:
        # Login
        print("  waiting for login prompt...")
        if not serial.wait_for("login: ", time.time() + BOOT_TIMEOUT):
            print("FAIL: login prompt not found")
            sys.exit(1)
        _type_string(mon, "root\n")
        if not serial.wait_for("assword", time.time() + 10):
            print("FAIL: password prompt not found")
            sys.exit(1)
        _type_string(mon, "forevervigilant\n")
        if not serial.wait_for("# ", time.time() + 10):
            print("FAIL: shell prompt not found")
            sys.exit(1)
        print("  logged in")
        time.sleep(1)

        # Test 1: Create a test file
        _type_string(mon, "echo hello > /home/testfile\n")
        time.sleep(1)

        # Test 2: Create symlink
        _type_string(mon, "ln -s /home/testfile /home/link1\n")
        time.sleep(1)
        _type_string(mon, "echo symlink_created\n")
        check("symlink creation", "symlink_created")

        # Test 3: readlink
        _type_string(mon, "readlink /home/link1\n")
        check("readlink output", "/home/testfile")

        # Test 4: Read through symlink
        _type_string(mon, "cat /home/link1\n")
        check("cat through symlink", "hello")

        # Test 5: Symlink chain
        _type_string(mon, "ln -s /home/link1 /home/link2\n")
        time.sleep(1)
        _type_string(mon, "cat /home/link2\n")
        check("symlink chain", "hello")

        # Test 6: chmod 000 then try to read
        _type_string(mon, "chmod 000 /home/testfile\n")
        time.sleep(1)
        _type_string(mon, "cat /home/testfile 2>&1\n")
        time.sleep(2)
        # cat should fail — look for error or empty output then prompt
        _type_string(mon, "echo chmod_test_done\n")
        check("chmod enforcement", "chmod_test_done")

        # Test 7: chmod restore
        _type_string(mon, "chmod 644 /home/testfile\n")
        time.sleep(1)
        _type_string(mon, "cat /home/testfile\n")
        check("chmod restore read", "hello")

        # Summary
        print(f"\n  Results: {passed} passed, {failed} failed")
        sys.exit(0 if failed == 0 else 1)

    finally:
        try:
            mon.close()
        except OSError:
            pass
        proc.kill()
        proc.wait()
        try:
            os.unlink(mon_path)
        except OSError:
            pass


if __name__ == "__main__":
    run_test()
```

- [ ] **Step 2: Add to run_tests.sh**

Add `test_symlink.py` to the test runner script if it exists, or note it should be run manually.

- [ ] **Step 3: Run make test (boot oracle)**

Run: `make -C /Users/dylan/Developer/aegis test 2>&1 | tail -10`
Expected: Boot oracle passes (no new [SUBSYSTEM] OK lines needed).

- [ ] **Step 4: Run test_symlink.py**

Run: `cd /Users/dylan/Developer/aegis && python3 tests/test_symlink.py`
Expected: All tests PASS.

- [ ] **Step 5: Commit**

```bash
git add tests/test_symlink.py
git commit -m "test: Phase 41 symlink + chmod smoke test"
```

---

### Task 7: Update CLAUDE.md build status + forward constraints

**Files:**
- Modify: `/Users/dylan/Developer/aegis/.claude/CLAUDE.md`

- [ ] **Step 1: Add Phase 41 to build status table**

Add row:
```
| Symlinks + chmod/chown (Phase 41) | ✅ | ext2 symlinks (fast+slow); chmod/chown/lchown; DAC enforcement; readdir DT_LNK |
```

- [ ] **Step 2: Add Phase 41 forward constraints section**

```markdown
## Phase 41 — Forward Constraints

**Phase 41 status: ✅ complete. make test PASS. test_symlink.py PASS.**

1. **No symlinks on ramfs/initrd.** Only ext2 supports symlinks. `/tmp`, `/run`, `/dev`, and initrd files cannot be symlink targets or sources within their own namespace.

2. **No hard links.** `link()` / `linkat()` not implemented. Only symlinks.

3. **Symlink depth limit is 8.** Returns `-ELOOP` beyond 8 levels. Sufficient for Aegis.

4. **Permission enforcement only on ext2.** ramfs, initrd, procfs, and device files are not permission-checked. They are kernel-internal resources gated by capabilities.

5. **No uid=0 bypass.** Root (uid=0) is subject to the same DAC checks as any other user. This is by design — Aegis has no ambient authority.

6. **`ext2_open_ex` uses 512-byte `resolved` buffer.** Paths longer than 511 bytes after symlink expansion are truncated. Sufficient for practical use.

7. **umask not applied to ext2_create.** `ext2_create` is called with hardcoded 0644. The process umask should be consulted.
```

- [ ] **Step 3: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs: Phase 41 build status + forward constraints"
```
