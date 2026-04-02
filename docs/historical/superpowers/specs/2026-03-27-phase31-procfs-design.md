# Phase 31: /proc Filesystem + VMA Tracking — Design Spec

## Goal

Add a capability-gated `/proc` virtual filesystem and per-process VMA tracking infrastructure. `/proc` provides process introspection (`/proc/[pid]/maps`, `status`, `stat`, `exe`, `cmdline`, `fd/`) and system-wide info (`/proc/meminfo`, `/proc/version`). VMA tracking records every user-space mapping so `/proc/[pid]/maps` is accurate and Phase 33 (dynamic linking) has the bookkeeping it needs.

## Architecture

### VMA Tracking

Each process owns a dynamically-allocated VMA table — a sorted array of `vma_entry_t` structs stored in a `kva_alloc_pages(1)` page (170 slots). The table tracks every user-space mapping: ELF segments, heap, stack, mmap regions, thread stacks, and guard pages.

**Type definition:**

```c
#define VMA_NONE         0
#define VMA_ELF_TEXT     1   /* PT_LOAD with PROT_EXEC */
#define VMA_ELF_DATA     2   /* PT_LOAD without PROT_EXEC */
#define VMA_HEAP         3   /* [brk_base..brk] */
#define VMA_STACK        4   /* user stack */
#define VMA_MMAP         5   /* anonymous mmap */
#define VMA_THREAD_STACK 6   /* thread stack (mmap'd by pthread_create) */
#define VMA_GUARD        7   /* guard page (PROT_NONE) */

typedef struct {
    uint64_t base;
    uint64_t len;
    uint32_t prot;    /* PROT_READ | PROT_WRITE | PROT_EXEC */
    uint8_t  type;    /* VMA_* constant */
    uint8_t  _pad[3];
} vma_entry_t;  /* 24 bytes */
```

**Process struct additions:**

```c
/* In aegis_process_t */
vma_entry_t *vma_table;     /* kva-allocated; NULL until first use */
uint32_t     vma_count;     /* number of valid entries */
uint32_t     vma_capacity;  /* max entries (170 per page) */
char         exe_path[256]; /* binary path, set at execve */
```

Total added to PCB: 8 + 4 + 4 + 256 = 272 bytes. PCB ~3752 + 272 = ~4024 bytes. Fits in one page.

**VMA operations (new file `kernel/mm/vma.c` + `vma.h`):**

- `vma_init(proc)` — allocate one kva page, set capacity to 170, count to 0.
- `vma_insert(proc, base, len, prot, type)` — insert entry sorted by base. Merge with adjacent entries if same prot+type.
- `vma_remove(proc, base, len)` — remove range. If the range partially overlaps an existing VMA, split it (one entry becomes two).
- `vma_update_prot(proc, base, len, new_prot)` — update permissions for range. Split entries at boundaries if needed.
- `vma_clear(proc)` — set count to 0 (called by execve).
- `vma_clone(dst_proc, src_proc)` — deep copy: allocate new kva page, memcpy entries. Used by fork.
- `vma_free(proc)` — free the kva page. Called on process exit / zombie reap.

For `clone(CLONE_VM)` (threads): share the `vma_table` pointer. Both parent and child point to the same page. Add a `uint32_t vma_refcount` field — incremented on clone, decremented on exit/exec. Only free the page when refcount hits 0. This mirrors `fd_table_t` refcounting.

Updated struct additions:

```c
vma_entry_t *vma_table;
uint32_t     vma_count;
uint32_t     vma_capacity;
uint32_t     vma_refcount;  /* 1 = sole owner; >1 = shared (CLONE_VM) */
char         exe_path[256];
```

**Callers that record VMAs:**

| Caller | Operation | VMA type |
|--------|-----------|----------|
| ELF loader (PT_LOAD) | `vma_insert` | `VMA_ELF_TEXT` or `VMA_ELF_DATA` |
| `proc_spawn` (user stack) | `vma_insert` | `VMA_STACK` |
| `sys_brk` (grow) | `vma_insert` or extend existing | `VMA_HEAP` |
| `sys_brk` (shrink) | shrink or remove | `VMA_HEAP` |
| `sys_mmap` | `vma_insert` | `VMA_MMAP` |
| `sys_munmap` | `vma_remove` | — |
| `sys_mprotect` | `vma_update_prot` | — |
| `sys_execve` | `vma_clear` + fresh inserts | — |
| `sys_fork` | `vma_clone` | — |
| `sys_clone(CLONE_VM)` | share pointer, increment refcount | — |
| Process exit | `vma_free` (if refcount==0) | — |

---

### /proc Filesystem

**New file: `kernel/fs/procfs.c` + `procfs.h`**

Registered as a VFS backend by path prefix. `vfs_open()` dispatches `/proc/*` paths to `procfs_open()`. `vfs_stat_path()` dispatches to `procfs_stat()`.

**Path routing:**

```
/proc/self/*        -> resolve "self" to current pid, dispatch to per-pid handler
/proc/<pid>/*       -> parse pid, look up process, dispatch to per-pid handler
/proc/meminfo       -> global: PMM stats
/proc/version       -> global: static version string
/proc/              -> directory listing (self, meminfo, version, plus all live pids)
/proc/<pid>/        -> directory listing (maps, status, stat, exe, cmdline, fd)
/proc/<pid>/fd/     -> directory listing (open fd numbers)
```

**Capability gating:**

- `/proc/self/*` — always allowed.
- `/proc/<pid>/*` where pid != caller's pid — requires `CAP_KIND_PROC_READ` (new kind = 10).
- `/proc/meminfo`, `/proc/version` — always allowed.
- Directory listings follow the same rules: `/proc/` listing shows all pids only if caller has `CAP_KIND_PROC_READ`; otherwise shows only `self`, `meminfo`, `version`, and caller's own pid.

**New capability:**

```c
#define CAP_KIND_PROC_READ  10u  /* may read /proc/[other-pid]/* */
```

Added to Rust `lib.rs` and C `cap.h`. Granted to vigil and shell via `exec_caps` baseline in `sys_execve`.

**File content generators:**

| File | Content |
|------|---------|
| `/proc/[pid]/maps` | Linux-compatible format: `<start>-<end> <perms> <offset> <dev> <inode> <name>`. One line per VMA entry. Perms = `r--p`, `rw-p`, `r-xp`, etc. Offset/dev/inode = `00000000 00:00 0`. Name = `[heap]`, `[stack]`, `[guard]`, or exe_path for ELF segments. |
| `/proc/[pid]/status` | Key-value pairs: `Name:`, `State:`, `Tgid:`, `Pid:`, `PPid:`, `Uid:`, `Gid:`, `VmSize:` (sum of all VMA lengths). |
| `/proc/[pid]/stat` | Single space-separated line: pid, comm, state char, ppid, pgid, 0 (session), 0 (tty), tgid, 0 (flags), ... (pad remaining fields with 0). |
| `/proc/[pid]/exe` | Plain text containing `exe_path`. (Not a symlink — Aegis has no symlinks yet.) |
| `/proc/[pid]/cmdline` | exe_path followed by NUL byte. (Full argv not stored; exe name only.) |
| `/proc/[pid]/fd/` | Directory listing: one entry per open fd (name = fd number as decimal string, type = DT_REG). |
| `/proc/meminfo` | `MemTotal: <n> kB\nMemFree: <n> kB\nMemAvailable: <n> kB\n`. Values from PMM page counts × 4. |
| `/proc/version` | `Aegis 0.31.0\n` |

**Implementation pattern:**

On `open()`, a generator function writes the file content into a kva-allocated buffer (one page, 4096 bytes max). The buffer pointer is stored in `vfs_file_t.priv`. The `read()` op copies from this buffer at the requested offset. `close()` frees the buffer via `kva_free_pages`.

This generate-on-open pattern avoids regenerating on every read (tools do multiple small reads) and avoids stale data across opens (each open gets a fresh snapshot).

For directory opens, `priv` stores a context struct with the pid (or 0 for root `/proc/`). The `readdir()` op enumerates entries dynamically.

**PMM stats:**

Need two functions exposed from `kernel/mm/pmm.c`:
- `pmm_total_pages()` — total managed pages
- `pmm_free_pages()` — count of free pages (scan bitmap or maintain a counter)

If these don't exist, add them. The bitmap allocator already has the info.

---

### Process List Iteration

`procfs_open()` for `/proc/<pid>/*` must find the process with that pid. The scheduler uses a circular linked list (`aegis_task_t.next`). To find a process by pid:

```c
aegis_process_t *proc_find_by_pid(uint32_t pid);
```

New function in `proc.c`. Walks the circular task list starting from `sched_current()`, returns the first `aegis_process_t` with matching pid, or NULL. Exposed via `proc.h`.

For `/proc/` directory listing, iterate the same list to collect all live pids.

**Safety:** Syscalls run with interrupts disabled on single-core. The task list is stable during a syscall. A process cannot exit while we're iterating. This is safe.

---

### Testing

**Test binary: `user/proc_test/main.c`**

1. Open `/proc/self/maps`, read contents, verify at least one line with `[stack]`.
2. Open `/proc/self/status`, verify `Pid:` line matches `getpid()`.
3. Open `/proc/meminfo`, verify `MemTotal:` line is present and non-zero.
4. Open `/proc/self/stat`, verify first field matches pid.
5. Open and readdir `/proc/self/fd/`, verify entries `0`, `1`, `2` exist.
6. Print `PROC OK` on success.

**Integration test: `tests/test_proc.py`**

Same pattern as `test_mmap.py`: q35 + NVMe disk, login, run `/bin/proc_test`, check for `PROC OK` in output.

**boot.txt:** Unchanged. No `[PROCFS] OK` line — procfs is a VFS backend, not a hardware subsystem (consistent with ramfs).

---

### Forward Constraints

1. **Shared VMA table needs refcounting.** Threads (`CLONE_VM`) share the table; refcount tracks ownership. Decrement on exit, free at zero. No lock needed on single-core.

2. **VMA table has no spinlock.** Safe single-core (syscalls are non-preemptible). SMP requires a per-table spinlock.

3. **`/proc/[pid]/exe` is a plain file, not a symlink.** Phase 38 (symlinks) can upgrade it to a proper symlink.

4. **cmdline stores exe name only, not full argv.** Full argv tracking requires storing argv at execve time (copy from user stack into kernel buffer). Deferred.

5. **PROT_NONE pages in VMA table.** `vma_update_prot` correctly tracks PROT_NONE guard pages. The VMA type is preserved even when perms change.

6. **`/proc/[pid]/fd/` entries are plain names, not symlinks to paths.** The fd number is the name; the target path is not available (VFS files don't store their path). Phase 38 could add target info.

7. **PMM free page count may require a running counter.** If the bitmap allocator doesn't maintain one, scanning the entire bitmap on every `/proc/meminfo` read is O(n) where n = total pages. For 128MB that's 32768 bits = fast enough. For larger memory, add a counter.
