# Phase 34: Writable Root — Design Spec

## Goal

Make the entire root filesystem coherent and writable. Currently the VFS merges three sources (initrd, ext2, ramfs) implicitly and incompletely — `ls /bin` misses initrd binaries, writes to non-ext2 non-ramfs paths fail silently, and the root directory listing is a hardcoded static table. After Phase 34, the system has one unified writable root that the installer (Phase 35) can snapshot to NVMe.

## Architecture

**ext2 becomes the primary root filesystem.** The initrd is demoted to a fallback for diskless boots. The VFS open order is reversed: ext2 first, initrd second. All files that were previously initrd-only are added to the ext2 disk image at build time. ramfs instances remain for volatile paths (`/tmp`, `/run`) and pseudo-filesystems (`/proc`, `/dev`).

This mirrors how real Linux systems work: initrd/initramfs bootstraps early boot, then the real root (on disk) takes over.

---

## Component 1: Disk Image — Add All Files to ext2

Currently the ext2 disk image (`make disk`) is missing the 6 static initrd binaries. These must be added so ext2 has a complete `/bin`:

Add to `make disk` debugfs commands:
- `write user/vigil/vigil /bin/vigil`
- `write user/login/login.elf /bin/login`
- `write user/shell/shell.elf /bin/sh` (already there, verify)

Also ensure all `/etc` files are on ext2 (most already are from `make disk`):
- `/etc/motd` — already written
- `/etc/passwd`, `/etc/shadow`, `/etc/group` — already written
- `/etc/hosts` — NOT on ext2 currently. Add it.
- `/etc/profile` — NOT on ext2. Add it.
- Vigil service configs — already written

Add `/tmp` and `/run` directories to ext2 (they exist as mount points).

Add `/root` directory to ext2 (already exists? verify).

---

## Component 2: VFS Open Order — ext2 First

**Current order:** procfs → ramfs(/run, /etc, /root) → initrd → ext2

**New order:** procfs → device specials → ext2 → ramfs(/tmp, /run) → initrd (fallback)

The key change in `vfs_open`:
1. `/proc/*` → procfs (unchanged)
2. `/dev/*` → device files (unchanged — ptmx, pts, console, etc.)
3. `/tmp/*` → ramfs (NEW — volatile tmpfs)
4. `/run/*` → ramfs (unchanged — volatile runtime state)
5. **Try ext2 first** for all other paths (`/bin/*`, `/etc/*`, `/lib/*`, `/root/*`, etc.)
6. **Initrd fallback** — only if ext2 open fails (ENOENT or not mounted)

This means:
- `/etc/passwd` opens from ext2 (writable, on disk) instead of ramfs shadow
- `/bin/cat` opens from ext2 (dynamically linked) instead of initrd
- `/bin/login` opens from ext2 (static copy there) instead of initrd
- On diskless boot (-machine pc, no NVMe), everything falls back to initrd

**Remove the /etc ramfs.** It was a workaround for /etc not being writable. Now /etc IS writable (it's on ext2). The `initrd_iter_etc` population step is no longer needed.

**Keep /root ramfs?** If /root is on ext2, it's persistent across reboots — that's probably what we want. Remove the /root ramfs too. Add `/root` directory to ext2 disk.

---

## Component 3: VFS stat_path — Unified

`vfs_stat_path` currently has a hardcoded list of directory paths that return synthetic stat results. Change it to:

1. `/proc/*` → procfs stat (unchanged)
2. `/dev/*` → device stat (unchanged)
3. `/tmp`, `/run` → synthetic dir stat (ramfs)
4. **Try ext2 stat** for everything else
5. **Initrd fallback** for stat

The hardcoded directory list (`/`, `/etc`, `/bin`, `/dev`, `/root`, `/run`, `/proc`) can be simplified: only `/dev`, `/proc`, `/tmp`, `/run` need special handling. All others (`/`, `/etc`, `/bin`, `/lib`, `/root`) are real ext2 directories.

---

## Component 4: Directory Listings — ext2 Based

Currently `ls /` shows a hardcoded `s_root_entries` from initrd. After this change:

- `ls /` → ext2 readdir on inode 2 (root directory). Shows all top-level dirs/files on disk.
- `ls /bin` → ext2 readdir (already works since Phase 33).
- `ls /etc` → ext2 readdir (will show all files including vigil configs).
- `ls /lib` → ext2 readdir (shows libc.so, ld-musl-x86_64.so.1).

The initrd's synthetic directory tables (`s_root_entries`, `s_etc_entries`, etc.) are only used when ext2 is not mounted (diskless fallback).

Add `/tmp`, `/run`, `/proc`, `/dev` directories to ext2 disk (as empty directories) so `ls /` shows them. Or handle them specially in the root directory listing by merging ext2 entries with the pseudo-filesystem mount points.

**Simplest approach:** Add empty `/tmp`, `/run`, `/proc`, `/dev`, `/lib`, `/root` directories to ext2 via `make disk`. Then `ls /` on ext2 shows everything. The pseudo-filesystems (procfs, devfs) are handled by VFS dispatch before ext2, so opening `/proc/self/maps` still works even though ext2 has an empty `/proc` directory.

---

## Component 5: /tmp ramfs

Add a new ramfs instance `s_tmp_ramfs` for `/tmp`. This is volatile storage that doesn't persist to disk. Programs that write temp files (the ext2 persistence test, vigil's PID files, etc.) use `/tmp`.

Initialize in `vfs_init()`:
```c
ramfs_init(&s_tmp_ramfs);
```

Dispatch in `vfs_open`:
```c
if (path[0]=='/' && path[1]=='t' && path[2]=='m' && path[3]=='p' &&
    (path[4]=='/' || path[4]=='\0'))
    return ramfs_open(&s_tmp_ramfs, path[4] ? path + 5 : "", flags, out);
```

Add `/tmp` to `vfs_stat_path` as a directory.

---

## Component 6: Initrd Simplification

With ext2 as the primary root, the initrd's role shrinks to:
1. **Diskless boot fallback** — everything in initrd is still accessible when no ext2 is mounted
2. **Early boot** — before ext2 mounts, vigil can still find its binary and configs in initrd

No files are removed from the initrd. The 20 files stay. But the VFS no longer checks initrd first — it's the last resort.

The `initrd_iter_etc` callback and the `/etc` ramfs population can be removed since `/etc` is served from ext2. The `/etc` ramfs instance (`s_etc_ramfs`) is removed.

The `/root` ramfs instance (`s_root_ramfs`) is removed — `/root` is an ext2 directory.

---

## Component 7: Boot Sequence Consideration

Current boot order in main.c:
1. `vfs_init()` — inits ramfs, populates /etc from initrd
2. Hardware init (ACPI, PCIe, NVMe, GPT)
3. `ext2_mount("nvme0p1")`
4. `proc_spawn_init()` — vigil starts

After Phase 34:
1. `vfs_init()` — inits /tmp and /run ramfs only (no /etc population needed)
2. Hardware init
3. `ext2_mount("nvme0p1")` — this is now when the real root becomes available
4. `proc_spawn_init()` — vigil starts, all files available via ext2

On diskless boot (no NVMe), step 3 is skipped. vigil and its configs come from initrd fallback. The system is functional but read-only and limited to initrd files.

---

## Testing

### Boot oracle (`make test`)
No changes to `tests/expected/boot.txt` — same subsystem OK lines. `[VFS] OK: initialized` still prints.

### test_integrated.py
All 16 tests should continue passing. The binaries are the same (on ext2), pipes/signals/stat all work the same way. The only difference is that `vfs_open` finds them via ext2 first instead of initrd first.

### test_ext2.py
Should continue passing — it boots INIT=shell (from initrd) with its own temp disk. ext2 mounts, echo/cat/ls found in initrd fallback (temp disk doesn't have them, but initrd does).

### New test: verify writability
Add a test to `test_integrated.py`:
- `touch /bin/test_write && ls /bin/test_write && rm /bin/test_write` — verify /bin is writable
- `echo test > /tmp/foo && cat /tmp/foo` — verify /tmp works

---

## Forward Constraints

1. **Diskless boot is degraded.** Without ext2, only initrd files are available (6 binaries + config). Dynamic binaries won't work (no /lib/libc.so). This is acceptable — diskless boot is for the boot oracle test only.

2. **No pivot_root or mount namespaces.** The VFS dispatch is a flat priority chain, not a mount tree. Phase 35 installer doesn't need these.

3. **ext2 writes are synchronous.** No write-back cache. Every write goes to disk immediately via NVMe. Performance is adequate for Phase 34 scope.

4. **ramfs has a 4KB per-file limit (RAMFS_MAX_SIZE).** `/tmp` files larger than 4KB will be truncated. Increase if needed.

5. **/tmp and /run are not persistent.** Contents lost on reboot. This is correct behavior.

6. **No /var.** Could add a ramfs for /var, or just let it be an ext2 directory. ext2 /var is persistent, which is correct for logs.
