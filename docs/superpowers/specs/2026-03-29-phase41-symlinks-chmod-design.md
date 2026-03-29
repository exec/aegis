# Phase 41: Symlinks + chmod/chown

**Date:** 2026-03-29
**Status:** Approved
**Depends on:** Phase 40 (complete)

---

## Goal

Add POSIX symlink support and file permission enforcement to Aegis. Symlinks
are a namespace feature (one path points to another). Permissions are an
access-control feature (mode bits gate read/write/execute per user/group/other).
Both are required for POSIX compatibility and real-world software.

---

## Scope

### In Scope

- ext2 symlink creation, reading, and following during path resolution
- `symlink()`, `readlink()`, `lstat()` syscalls
- `chmod()`, `fchmod()`, `chown()`, `fchown()`, `lchown()` syscalls
- Permission enforcement in `sys_open`, `sys_access`, `sys_unlink`, `sys_rename`, `sys_mkdir`, `sys_execve`
- Defense-in-depth: capabilities gate the syscall, then mode bits gate the file

### Out of Scope

- VFS-level path walk refactor (ext2 handles symlinks internally)
- ramfs/initrd symlink or permission support (kernel-internal, flat)
- Hard links (`link()` / `linkat()`)
- ACLs or extended attributes
- `symlinkat()` / `readlinkat()` (at-variants deferred)

---

## Design

### 1. New Syscalls

| Syscall | Nr (x86-64) | Signature | Cap Gate |
|---------|-------------|-----------|----------|
| `symlink` | 88 | `symlink(const char *target, const char *linkpath)` | `CAP_KIND_VFS_WRITE` |
| `readlink` | 89 | `readlink(const char *path, char *buf, size_t bufsiz) -> ssize_t` | `CAP_KIND_VFS_READ` |
| `chmod` | 90 | `chmod(const char *path, mode_t mode)` | `CAP_KIND_VFS_WRITE` |
| `fchmod` | 91 | `fchmod(int fd, mode_t mode)` | `CAP_KIND_VFS_WRITE` |
| `chown` | 92 | `chown(const char *path, uid_t uid, gid_t gid)` | `CAP_KIND_SETUID` |
| `fchown` | 93 | `fchown(int fd, uid_t uid, gid_t gid)` | `CAP_KIND_SETUID` |
| `lchown` | 94 | `lchown(const char *path, uid_t uid, gid_t gid)` | `CAP_KIND_SETUID` |

`lstat` (nr 6) is already dispatched but currently aliases `stat`. It must be
changed to not follow the final symlink component.

### 2. ext2 Symlink Support

#### 2.1 Constants

```c
#define EXT2_S_IFLNK     0xA000   /* symlink inode type */
#define EXT2_FT_SYMLINK   7       /* directory entry file type */
#define SYMLINK_MAX_DEPTH 8       /* max symlink following depth */
```

#### 2.2 Symlink Creation (`ext2_symlink`)

```
int ext2_symlink(const char *linkpath, const char *target);
```

1. Split `linkpath` into parent directory + name component.
2. Allocate a new inode with `i_mode = EXT2_S_IFLNK | 0777`.
3. If `strlen(target) <= 60`: **fast symlink** -- store target directly in
   the inode's `i_block[0..14]` array (60 bytes of inline storage). No block
   allocation needed.
4. If `strlen(target) > 60`: **slow symlink** -- allocate a data block via
   `ext2_alloc_block`, write target into it, set `i_block[0]` to the block
   number.
5. Set `i_size = strlen(target)`. Set `i_links_count = 1`.
6. Add directory entry in parent with `file_type = EXT2_FT_SYMLINK`.

#### 2.3 Symlink Reading (`ext2_readlink`)

```
int ext2_readlink(const char *path, char *buf, uint32_t bufsiz);
```

1. Resolve `path` to an inode **without following the final component**.
2. Verify `(i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK`. Return `-EINVAL` if not.
3. Read target: if `i_size <= 60`, copy from `i_block[]`; else read data
   block `i_block[0]`.
4. Copy `min(i_size, bufsiz)` bytes to `buf`. Return bytes copied.

#### 2.4 Symlink Following in Path Walk

The `ext2_open` function does a component-by-component directory walk starting
from inode 2 (root). After resolving each component to an inode:

1. Check if the inode is a symlink: `(i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK`.
2. If it is the **final** component and the caller requested no-follow
   (lstat/readlink/lchown), stop -- return this inode.
3. Otherwise, read the symlink target.
4. Increment depth counter. If `depth > SYMLINK_MAX_DEPTH`, return `-ELOOP`.
5. If target starts with `/`: restart walk from root inode 2.
6. If target is relative: restart walk from the current parent directory inode.
7. Continue resolving the remaining path components.

The `ext2_open` signature gains a `flags` parameter (or a separate
`ext2_open_nofollow` variant) to control final-component symlink following.

#### 2.5 Symlink Deletion

`ext2_unlink` already removes directory entries and decrements `i_links_count`.
For symlinks: if `i_links_count` drops to 0 and `i_size > 60`, free the data
block. The inode is freed by the existing logic.

### 3. Permission Enforcement

#### 3.1 Permission Check Function

```c
int ext2_check_perm(uint16_t i_mode, uint16_t i_uid, uint16_t i_gid,
                    uint16_t proc_uid, uint16_t proc_gid, int want);
```

- `want` is a bitmask: `R_OK=4`, `W_OK=2`, `X_OK=1` (matching POSIX).
- Extract the relevant 3-bit permission field:
  - If `proc_uid == i_uid`: use bits 8-6 (owner).
  - Else if `proc_gid == i_gid`: use bits 5-3 (group).
  - Else: use bits 2-0 (other).
- Return 0 if `(extracted & want) == want`, else `-EACCES`.

**No root bypass.** `uid=0` is subject to the same permission checks as any
other uid. The Aegis security model has no ambient authority -- capabilities
and mode bits are independent layers, both must pass.

#### 3.2 Enforcement Points

| Syscall | Check | Details |
|---------|-------|---------|
| `sys_open` | Cap check first, then DAC | `O_RDONLY` requires `R_OK`. `O_WRONLY`/`O_RDWR` requires `W_OK`. `O_CREAT` on existing file: `W_OK` on parent dir. |
| `sys_access` | DAC only (already past cap gate) | Check real uid/gid against requested mode. Return `-EACCES` on failure. |
| `sys_unlink` | `W_OK + X_OK` on parent directory | Must be able to write to and traverse the parent. |
| `sys_rename` | `W_OK + X_OK` on both parent dirs | Source parent and destination parent. |
| `sys_mkdir` | `W_OK + X_OK` on parent directory | Same as unlink. |
| `sys_execve` | `X_OK` on the ELF file | Checked before loading. |
| `sys_chdir` | `X_OK` on the directory | Must be able to traverse. |

Directory traversal: each directory component in the path walk should
require `X_OK`. This is checked inside `ext2_open`'s component-by-component
walk for each intermediate directory.

#### 3.3 Process uid/gid

Aegis processes already have `uid` and `gid` fields in the process struct
(set by `sys_setuid`/`sys_setgid`). All processes currently run as uid=0,
gid=0. The permission check function uses these fields. Since uid=0 gets no
special bypass, permissions work identically for all users.

### 4. chmod / chown Implementation

#### 4.1 ext2_chmod

```c
int ext2_chmod(const char *path, uint16_t mode);
```

1. Resolve path to inode (following symlinks).
2. Update: `i_mode = (i_mode & EXT2_S_IFMT) | (mode & 0x1FF)`.
3. Write inode back to disk.

#### 4.2 ext2_chown

```c
int ext2_chown(const char *path, uint16_t uid, uint16_t gid);
```

1. Resolve path to inode (following symlinks for `chown`, not for `lchown`).
2. Update `i_uid = uid`, `i_gid = gid`.
3. Write inode back to disk.

Gated on `CAP_KIND_SETUID` -- only processes with this capability can change
file ownership.

#### 4.3 fchmod / fchown

Operate on an open file descriptor. The ext2 VFS file private data must
include the inode number so we can read, modify, and write back the inode.
Currently `ext2_open` stores `ext2_file_priv_t` with `{ino, size}` -- this
already contains the inode number.

### 5. VFS Layer Changes

Minimal changes to the VFS dispatch layer:

1. **`vfs_stat_path`** gains a `follow_symlink` flag. When false (lstat), the
   ext2 backend resolves the path without following the final component.

2. **`vfs_open`** continues to follow symlinks by default (standard POSIX
   `open` behavior). An `O_NOFOLLOW` flag can be added later if needed.

3. **New VFS functions:**
   - `vfs_symlink(target, linkpath)` -- dispatches to ext2 if linkpath is on ext2.
   - `vfs_readlink(path, buf, bufsiz)` -- dispatches to ext2.
   - `vfs_chmod(path, mode)` -- dispatches to ext2.
   - `vfs_chown(path, uid, gid, follow)` -- dispatches to ext2. `follow=0` for lchown.

4. **Permission checks** are inserted into `vfs_open` and the ext2 path walk,
   not into every filesystem backend. ramfs and initrd are not permission-checked
   (they are kernel-internal resources gated by capabilities).

### 6. What Does NOT Change

- **ramfs:** No symlinks, no permission enforcement. Files remain mode 0644.
- **initrd:** No symlinks, no permission enforcement. Files remain mode 0555.
- **procfs:** No changes.
- **Capability system:** No new capability kinds. Reuse `CAP_KIND_SETUID` for
  chown, `CAP_KIND_VFS_WRITE` for chmod/symlink.
- **VFS dispatch architecture:** Still prefix-based. No mount table refactor.

### 7. Testing

**Boot oracle (`make test`):** No new `[SUBSYSTEM] OK` lines expected. Phase 41
is a feature addition, not a new subsystem init. Existing boot.txt unchanged.

**`test_symlink.py`:** QEMU q35 boot with NVMe disk, exercises:

1. Create symlink: `ln -s /etc/motd /home/link1` -- verify exit 0
2. Readlink: `readlink /home/link1` -- verify output is `/etc/motd`
3. Read through symlink: `cat /home/link1` -- verify content matches `/etc/motd`
4. lstat vs stat: `stat /home/link1` shows symlink type, dereferenced stat shows regular file
5. Symlink chain: create `link2 -> link1 -> /etc/motd`, cat through chain
6. ELOOP: create circular symlinks, verify error
7. chmod: `chmod 000 /home/testfile`, attempt to read -- verify EACCES
8. chmod restore: `chmod 644 /home/testfile`, read succeeds
9. Permission enforcement: create file as root, verify mode bits gate access

### 8. User-Space Tools

The shell built-ins and `/bin/ln` (if present) should be usable. If `ln` is
not in the initrd, a minimal `ln` binary (or shell `ln -s` support) is needed
for testing. `chmod` and `chown` similarly.

---

## Risks

1. **Permission enforcement breaks existing software.** Current files are
   created with mode 0644/0755 and all processes run as uid=0, so owner bits
   are checked. Risk is low -- owner bits for 0644 allow read+write, 0755
   allows read+write+execute.

2. **ext2 path walk complexity.** Symlink following adds loops and recursion
   to the path walk. Depth limit (8) prevents infinite loops. Stack usage is
   bounded.

3. **ext2 inode i_uid/i_gid are 16-bit.** ext2 revision 0 uses 16-bit
   uid/gid. Sufficient for Aegis v1.
