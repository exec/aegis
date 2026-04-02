# Audit 5: Filesystem Integrity

## Priority: HIGH

ext2 is read-write with no journal. Any crash corrupts the filesystem.
The VFS merges three data sources (initrd, ext2, ramfs) with implicit priority.

## Files to review

| File | LOC | Focus |
|------|-----|-------|
| `kernel/fs/ext2.c` | 1177 | Read-write ext2, block cache, symlinks, directories |
| `kernel/fs/vfs.c` | 590 | VFS dispatch, open order, stat, permission checks |
| `kernel/fs/initrd.c` | 567 | Static initrd, synthetic root directory |
| `kernel/fs/ramfs.c` | 260 | In-memory writable FS for /etc, /root |
| `kernel/fs/procfs.c` | 807 | /proc virtual filesystem |
| `kernel/fs/pipe.c` | 263 | Pipe ring buffer |
| `kernel/tty/pty.c` | 521 | PTY master/slave |
| `kernel/fs/gpt.c` | 277 | GPT partition parsing, CRC validation |
| `kernel/fs/memfd.c` | 245 | memfd_create, ftruncate |

## Checklist

### ext2 corruption resilience
- [ ] Block cache flushes on sync (ext2_sync called on power button)
- [ ] Directory entry manipulation doesn't leave orphan inodes
- [ ] Symlink creation atomic (no partial fast symlink)
- [ ] Double indirect block reads validated (bounds check on block numbers)
- [ ] Inode bitmap and block bitmap updates consistent
- [ ] ext2_create doesn't leak blocks on partial failure

### Path traversal
- [ ] Symlink depth enforced (limit 8)
- [ ] No escape from intended directory via `../../..`
- [ ] Null bytes in paths rejected
- [ ] Path buffer overflow (`ext2_open_ex` 512-byte resolved buffer)

### Permission enforcement
- [ ] DAC checks on ext2 open/read/write/exec
- [ ] No bypass via initrd fallback (open via initrd skips ext2 permissions)
- [ ] Boot-time opens correctly skip DAC (is_user check)

### VFS consistency
- [ ] fd table bounds checked on every access
- [ ] close() doesn't double-free VFS resources
- [ ] dup/dup2 refcounting correct
- [ ] O_CLOEXEC respected on exec (or documented as deferred)

## Output format

Same as Audit 1.
