# Phase 26-Prep: ramfs, /root, sys_chdir Validation, vigictl Fix

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix four bare-metal reliability issues before implementing the socket API: add an in-memory ramfs for `/run`, create `/root` in the initrd, validate paths in `sys_chdir`, and fix vigictl to tolerate a missing PID file.

**Architecture:** Minimal targeted fixes — no new subsystems except ramfs. ramfs is a small kernel VFS driver (16-file table, kva-backed buffers) registered at mount time. `sys_chdir` gains a `vfs_stat_path` call to validate the target. vigictl falls back to a `/dev/vigil.cmd` virtual device if `/run/vigil.pid` is absent.

**Tech Stack:** C kernel, existing VFS ops vtable, kva allocator, static C initrd (not cpio).

---

## Constraints and Non-Negotiables

- ramfs must work without ext2 disk. It must be the first writable filesystem available to all processes including PID 1.
- ramfs is not persistent. Files are lost on reboot. That is correct behavior for `/run`.
- ramfs files are kva-backed. No PMM page allocation — the kva bump allocator is sufficient for small PID files and cmd pipes.
- `sys_chdir` must reject non-existent paths with `-ENOENT` after this patch. It may call `vfs_stat_path` which already handles initrd, ext2, and `/dev/` paths. If `vfs_stat_path` returns `-ENOENT` the chdir fails.
- `/root` directory must appear in initrd (cpio archive, created at build time by Makefile). It does not need to be writable — it just needs to exist so oksh can `cd /root` at startup.
- vigictl fix is userspace only — no new syscalls. The simplest fix: if `/run/vigil.pid` does not open successfully, print `vigil: not running (no pid file)` and exit. The cmd-writing path must also check the open return value and fail gracefully.
- `make test` must still pass with zero changes to `tests/expected/boot.txt`.

---

## File Layout

| File | Action | Purpose |
|------|--------|---------|
| `kernel/fs/ramfs.c` | Create | ramfs VFS driver — 16-file table, kva-backed |
| `kernel/fs/ramfs.h` | Create | `ramfs_init()`, `ramfs_mkdir()` declarations |
| `kernel/fs/vfs.c` | Modify | Call `ramfs_init()` in `vfs_init()`; add `/run/` + `/run` + `/root` + `/dev` to `vfs_open`/`vfs_stat_path` |
| `kernel/fs/initrd.c` | Modify | Add `/root` dir entry to `s_root_entries[]`; add `s_root_dir_entries[]`; register in `dir_paths[]`/`dir_tables[]` |
| `kernel/syscall/sys_file.c` | Modify | `sys_chdir`: call `vfs_stat_path` + `S_IFDIR` check before updating `proc->cwd` |
| `user/vigictl/main.c` | Modify | Graceful error if `/run/vigil.pid` cannot be opened |

---

## ramfs Design

### Data Structures

```c
/* kernel/fs/ramfs.h */
#define RAMFS_MAX_FILES   16
#define RAMFS_MAX_NAMELEN 64
#define RAMFS_MAX_SIZE    4096  /* one kva page per file */

void ramfs_init(void);
/* ramfs_open: open or create (O_CREAT) a file under /run by its short name.
 * Returns 0 on success and fills *out; -2 (ENOENT) if not found and O_CREAT not set. */
int  ramfs_open(const char *name, int flags, vfs_file_t *out);
/* ramfs_stat_path: fill *st for a ramfs file given its short name (no "/run/" prefix).
 * e.g. call as ramfs_stat_path("vigil.pid", st). Returns 0 on success, -2 if not found. */
int  ramfs_stat_path(const char *name, k_stat_t *st);
```

```c
/* kernel/fs/ramfs.c */
typedef struct {
    char     name[RAMFS_MAX_NAMELEN];  /* path relative to /run — e.g. "vigil.pid" */
    uint8_t *data;                     /* kva-allocated page, NULL if not yet written */
    uint32_t size;                     /* current byte count */
    uint8_t  in_use;                   /* 1 if slot is occupied */
} ramfs_file_t;

static ramfs_file_t s_ramfs[RAMFS_MAX_FILES];
```

### VFS ops_t Implementation

ramfs exposes a standard `vfs_ops_t`:

```c
static const vfs_ops_t s_ramfs_ops = {
    .read    = ramfs_read,
    .write   = ramfs_write,
    .close   = ramfs_close,
    .readdir = ramfs_readdir,
    .dup     = ramfs_dup,
    .stat    = ramfs_stat,
};
```

**`ramfs_write`**: On first write, allocates one kva page (`kva_alloc_pages(1)`). Writes up to `RAMFS_MAX_SIZE` bytes. Overwrites from offset 0 (O_TRUNC semantics — sufficient for PID files and short command strings).

**`ramfs_read`**: Returns bytes from `data` buffer starting at `off`.

**`ramfs_close`**: No-op (data persists until process end or explicit truncation).

**`ramfs_readdir`**: Iterates `s_ramfs[]` and returns in-use filenames at the given index.

**`ramfs_stat`**: Fills `k_stat_t` with `S_IFREG | 0644`, `st_size = file->size`.

### Path Resolution in vfs_open / vfs_stat_path

`vfs_open` and `vfs_stat_path` have hardcoded path chains. Both must be updated.

**`vfs_open`** (in `kernel/fs/vfs.c`): Add a check for `/run/` before the ext2 fallback:

```c
/* In vfs_open, before the ext2 fallback */
if (strncmp(path, "/run/", 5) == 0) {
    return ramfs_open(path + 5, flags, out);
}
```

`ramfs_open` finds an existing entry in `s_ramfs[]` by name, or allocates a new slot if `flags & VFS_O_CREAT`. Returns `-2` (ENOENT) for missing files opened without `O_CREAT`.

**`vfs_stat_path`** (in `kernel/fs/vfs.c`, currently line 252): The existing hardcoded directory check is:

```c
if (streq(path, "/") || streq(path, "/etc") || streq(path, "/bin")) {
```

Extend it to include `/root`, `/dev`, and `/run`:

```c
if (streq(path, "/")    || streq(path, "/etc") || streq(path, "/bin") ||
    streq(path, "/dev") || streq(path, "/root") || streq(path, "/run")) {
```

Also add a ramfs file lookup before the ext2 fallback:

```c
/* ramfs lookup for /run/ paths — pass name after "/run/" prefix (matches ramfs_open convention) */
if (strncmp(path, "/run/", 5) == 0) {
    return ramfs_stat_path(path + 5, out);
}
```

`ramfs_stat_path` receives the short name (e.g., `"vigil.pid"`) — the same convention as `ramfs_open`. It looks up the entry in `s_ramfs[]` by name and fills `*out`. Returns `-2` (ENOENT) if the file has not been created yet.

---

## sys_chdir Fix

Current implementation blindly copies the path into `proc->cwd` with no validation.

Fix: call `vfs_stat_path` first. If it returns non-zero, return `-ENOENT`.

```c
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

    /* Validate path exists */
    k_stat_t st;
    if (vfs_stat_path(kpath, &st) != 0) return (uint64_t)-(int64_t)2; /* ENOENT */
    /* Must be a directory */
    if ((st.st_mode & S_IFMT) != S_IFDIR) return (uint64_t)-(int64_t)20; /* ENOTDIR */

    /* Copy into proc->cwd */
    for (i = 0; i < 256; i++) {
        proc->cwd[i] = kpath[i];
        if (kpath[i] == '\0') break;
    }
    return 0;
}
```

### vfs_stat_path directory paths that must work

`vfs_stat_path` must already handle these (check before adding ramfs support):
- `/` — root (always exists)
- `/bin`, `/etc` — initrd top-level directories
- `/root` — initrd directory (added by this phase)
- `/run` — ramfs mount point (added by this phase)
- `/dev` — synthetic

---

## /root in initrd

The initrd is **not** a cpio archive. It is a statically-compiled C file (`kernel/fs/initrd.c`) with hardcoded directory entry arrays. There is no find/cpio pipeline in the Makefile.

To add `/root` as an empty directory visible to the VFS, three edits to `kernel/fs/initrd.c` are needed:

**1. Add `root` to `s_root_entries[]`** (line 219–221):

```c
static const dir_entry_t s_root_entries[] = {
    { "etc", 4 }, { "bin", 4 }, { "dev", 4 }, { "root", 4 },
    { (const char *)0, 0 }
};
```

**2. Add an empty sentinel array for `/root`'s contents** (new, after `s_dev_entries`):

```c
static const dir_entry_t s_root_dir_entries[] = {
    { (const char *)0, 0 }   /* /root is empty */
};
```

**3. Register `/root` in the `dirs[]` and `dir_tables[]` local arrays** inside `initrd_open()` (currently around line 306–317). These are **local stack variables** (not static file-scope arrays):

```c
/* existing local arrays — add "/root" and s_root_dir_entries: */
const char *dirs[9] = {
    "/", "/etc", "/bin", "/dev",
    "/etc/vigil", "/etc/vigil/services", "/etc/vigil/services/getty",
    "/root",          /* new */
    (const char *)0
};
const dir_entry_t *dir_tables[9] = {
    s_root_entries, s_etc_entries, s_bin_entries, s_dev_entries,
    s_vigil_entries, s_vigil_services_entries, s_vigil_getty_entries,
    s_root_dir_entries,   /* new */
    (const dir_entry_t *)0
};
```

Update the loop bound from `d < 7` to `d < 8` accordingly. The array sizes must also grow from `[8]` to `[9]` to accommodate the new entry plus the null sentinel.

The directory only needs to exist — it does not need any contents. oksh sets `HOME=/root` and tries `cd /root` at startup; with these edits and the `sys_chdir` + `vfs_stat_path` fixes, the shell starts in `/root` correctly.

---

## vigictl Fix

`user/vigictl/main.c` opens `/run/vigil.pid` to find the vigil PID, and `/run/vigil.cmd` to send commands. With ramfs at `/run`, vigil can write its PID file correctly.

However, on a fresh boot before vigil has started, or on bare metal without the disk, the PID file may not exist. Fix vigictl to print a clear error:

```c
static int
read_vigil_pid(void)
{
    int fd = open("/run/vigil.pid", O_RDONLY);
    if (fd < 0) {
        fputs("vigil: not running (no pid file at /run/vigil.pid)\n", stderr);
        return -1;
    }
    /* ... existing read logic ... */
}
```

Similarly for `write_cmd()`:

```c
static int
write_cmd(const char *cmd)
{
    int fd = open("/run/vigil.cmd", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        fputs("vigil: cannot open /run/vigil.cmd\n", stderr);
        return -1;
    }
    /* ... existing write logic ... */
}
```

**Important:** The existing vigictl code at line ~59 already prints `"vigictl: vigil not running\n"` to fd 2 when `read_vigil_pid()` returns `-1`. The fix replaces the **silent** `return -1` inside `read_vigil_pid()` with a message-bearing return so the caller gets a useful message regardless of context. Do not add a second print path — verify that only one error message is emitted when the PID file is absent.

No changes to vigil itself — vigil already attempts to write `/run/vigil.pid` at startup. With ramfs present it will succeed.

---

## vfs_stat_path — Directory Entries

`vfs_stat_path` must synthesize stat results for directory paths. Audit the existing implementation to ensure it handles:

| Path | Expected result |
|------|----------------|
| `/` | `S_IFDIR \| 0755`, ino=1 |
| `/bin` | `S_IFDIR \| 0755` |
| `/etc` | `S_IFDIR \| 0755` |
| `/dev` | `S_IFDIR \| 0755` |
| `/root` | `S_IFDIR \| 0755` (initrd entry) |
| `/run` | `S_IFDIR \| 0755` (ramfs mount) |
| `/run/vigil.pid` | `S_IFREG \| 0644` when file exists in ramfs |

---

## Testing

`make test` must pass unchanged (no new boot output lines).

Manual smoke test on bare metal:
1. `cd /root` — succeeds, prompt shows `/root#`
2. `cd /nonexistent` — shell prints `cd: /nonexistent: No such file or directory`
3. `vigictl list` — shows vigil service list (no "not running" error)
4. `/run/vigil.pid` readable after vigil starts

---

## Forward-Looking Constraints

- ramfs has no locking. Single-threaded kernel — safe. SMP requires a spinlock around `s_ramfs[]` access.
- `RAMFS_MAX_SIZE = 4096` is sufficient for PID files (< 10 bytes) and cmd strings (< 64 bytes). Larger writes are truncated. If the socket API's Unix domain sockets later use ramfs, this limit must be raised.
- `RAMFS_MAX_FILES = 16` is sufficient for Phase 26-prep. The socket API may need more entries if it creates ramfs-backed sockets — revisit at Phase 26.
- ramfs does not implement `rename` or `unlink`. vigil always opens with `O_CREAT | O_TRUNC`, so no rename needed for PID file update.
