# Phase 10: VFS + sys_read + Cleanup

## Goal

Five related items accumulated across Phases 5–9, plus the first real I/O path:

1. **`stack_pages` field** — replace the `is_user ? 1 : STACK_PAGES` inference in
   `sched_exit` with an explicit `stack_pages` field in `aegis_task_t`.
2. **`task_idle`** — rename `task_heartbeat` to `task_idle`; make it a true idle
   loop; move shutdown detection into `sched_exit` (triggered when the last user
   process exits).
3. **`copy_to_user`** — add the symmetric counterpart to `copy_from_user` in
   `kernel/mm/uaccess.h`; required by `sys_read`.
4. **`vmm_free_user_pml4`** — walk PML4 entries 0–255 and free all mapped
   physical frames plus intermediate page-table pages; called from `sched_exit`
   when a user process exits.
5. **VFS + `sys_open`/`sys_read`/`sys_close`** — file-ops vtable, per-process fd
   table, static initrd driver, and three new syscalls. Updated `user/init/main.c`
   opens `/etc/motd`, reads it, prints it, exits. `make test` oracle updated.

---

## Background

### Why `stack_pages` matters

`sched_exit` currently computes `dying->is_user ? 1 : STACK_PAGES` to determine
how many KVA pages to free. This is an undocumented invariant: a kernel task with
a stack size other than `STACK_PAGES` would silently free the wrong number of
pages. An explicit `stack_pages` field recorded at allocation time eliminates the
inference and its hidden contract.

### Why `task_idle` matters

`task_heartbeat` exits after 500 ticks and calls `arch_request_shutdown()`. This
means:
- Its own TCB is recorded in `g_prev_dying_tcb` and freed only if a subsequent
  `sched_exit` call fires — which it never does (kbd and idle never exit). One
  permanent 4-page leak per boot.
- The shutdown trigger is a wall-clock countdown, not a semantic condition.

With a true idle task that never exits, and shutdown detection moved into
`sched_exit` (fires when no `is_user` tasks remain in the run queue), the shutdown
is semantically correct: the kernel halts when the last user process exits.

### Why `vmm_free_user_pml4` matters

`sched_exit` currently leaks the user PML4 and all ELF segment pages. Each user
process exit permanently consumes a variable number of physical pages. Phase 10
reclaims them.

**ELF KVA dangling PTEs (accepted Phase 10 debt):** The ELF loader allocates KVA
pages via `kva_alloc_pages`, copies ELF segment data into them, then maps the
same physical frames into the user PML4. The KVA VAs are local variables in the
ELF loader and are lost after loading. When `vmm_free_user_pml4` frees the
physical frames, the KVA PTEs become stale. This is safe because:
- The KVA VAs are never accessed after ELF loading.
- The KVA bump allocator never rewinds; those addresses will never be remapped.
- The stale KVA PTEs are kernel-only (no USER flag); ring-3 code cannot reach
  them.

### Why the VFS matters

The kernel has `sys_write` but no `sys_read`. Without a file abstraction there is
nothing to read from. Phase 10 introduces the minimum VFS needed for a real I/O
round-trip: a file-ops vtable, a per-process fd table, a static initrd driver,
and three syscalls. No mount table, no directory tree, no dentry cache — just
enough to open a named file, read it, and close it.

---

## Architecture

### Cleanup Item 1: `stack_pages` field in `aegis_task_t`

Add to `sched.h` (after `is_user`):

```c
uint64_t stack_pages; /* kva pages allocated for this task's kernel stack */
```

Populate in `sched_spawn`:
```c
task->stack_pages = STACK_PAGES;
```

Populate in `proc_spawn` (in `proc.c`, when setting up the process TCB):
```c
proc->task.stack_pages = 1;
```

In `sched_exit`, replace `dying->is_user ? 1 : STACK_PAGES` with
`dying->stack_pages`:
```c
g_prev_dying_stack_pages = dying->stack_pages;
```

### Cleanup Item 2: `task_idle` + shutdown detection in `sched_exit`

**`kernel/core/main.c`** — rename `task_heartbeat` to `task_idle`. Replace its
body with a true idle loop:

```c
static void
task_idle(void)
{
    __asm__ volatile ("sti");
    for (;;)
        __asm__ volatile ("hlt");
}
```

Remove the `arch_request_shutdown()` call and the `arch_get_ticks()` loop.
The `[AEGIS] System halted.` print and shutdown trigger move to `sched_exit`.

Change `sched_spawn(task_heartbeat)` to `sched_spawn(task_idle)`.

**`kernel/sched/sched.c`** — in `sched_exit`, after removing `dying` from the
run queue (after `s_task_count--`) and **before** the existing
`if (s_current == dying)` last-task guard, insert:

```c
if (dying->is_user) {
    aegis_process_t *proc = (aegis_process_t *)dying;
    vmm_free_user_pml4(proc->pml4_phys);

    /* If no user tasks remain, request halt (deferred to next PIT tick). */
    int has_user = 0;
    aegis_task_t *t = s_current;
    do {
        if (t->is_user) { has_user = 1; break; }
        t = t->next;
    } while (t != s_current);

    if (!has_user) {
        printk("[AEGIS] System halted.\n");
        arch_request_shutdown();
        /* Continue — ctx_switch to idle/kbd; PIT tick exits QEMU. */
    }
}
```

**Retain the existing `if (s_current == dying)` guard** that follows this block.
That guard handles the degenerate case where `dying` was the absolute last task
(no kernel tasks remain), e.g. if a kernel task calls `sched_exit`. It is a
backstop, not the primary shutdown path for Phase 10.

This replaces the `[AEGIS] System halted.` print that was in `task_heartbeat`.
`sched_exit` already includes `proc.h` and `vmm.h`. `sched_exit` already switches
to master PML4 at entry — `vmm_free_user_pml4` runs safely after that switch.

**`sched_current()` accessor** — `syscall.c` needs the current task to reach the
fd table. Add to `sched.h`:

```c
/* Return a pointer to the currently running task. */
aegis_task_t *sched_current(void);
```

In `sched.c`:
```c
aegis_task_t *sched_current(void) { return s_current; }
```

### Cleanup Item 3: `copy_to_user` in `kernel/mm/uaccess.h`

Append after `copy_from_user`:

```c
/* copy_to_user — copy len bytes from kernel-space src to user-space dst.
 *
 * Caller MUST validate [dst, dst+len) with user_ptr_valid() before calling.
 * Single arch_stac/arch_clac window around the entire copy.
 * Same fault-recovery caveats as copy_from_user: no extable, caller must
 * ensure the range is mapped. */
static inline void
copy_to_user(void *dst, const void *src, uint64_t len)
{
    arch_stac();
    __builtin_memcpy(dst, src, len);
    arch_clac();
}
```

### Cleanup Item 4: `vmm_free_user_pml4` in `vmm.c` + `vmm.h`

**Declaration in `vmm.h`:**

```c
/* vmm_free_user_pml4 — walk PML4 entries 0–255 (user half) and free:
 *   - all leaf physical frames (PT entries)
 *   - all intermediate page-table pages (PT, PD, PDPT)
 *   - the PML4 page itself
 *
 * MUST NOT touch PML4 entries 256–511 (kernel half): those pages are shared
 * with the master PML4; freeing them corrupts every other process and the
 * kernel itself.
 *
 * Uses the mapped-window allocator for all page-table accesses.
 * Caller must have switched to the master PML4 before calling (sched_exit
 * does this at entry). Single-CPU only: no TLB shootdown. */
void vmm_free_user_pml4(uint64_t pml4_phys);
```

**Implementation in `vmm.c`:**

Use the per-entry window pattern: map once per entry, read one entry, unmap, then
use the value for the next level. No large local arrays on the stack.

**Why per-entry and not walk-overwrite:** The Phase 6 constraint against calling
`vmm_window_unmap()` between levels applies to single-path walks (one entry per
level, four `invlpg` total). `vmm_free_user_pml4` must iterate ALL entries at
each level, making the walk-overwrite pattern inapplicable. The alternative —
reading a full 512-entry table into a local `uint64_t[512]` array (4 KB) before
recursing — would nest three such arrays on the kernel stack (PDPT + PD + PT =
12 KB), nearly filling the 16 KB kernel stack. The per-entry approach uses only
a handful of stack locals at any nesting depth, at the cost of map/unmap per
entry. For the Phase 10 init binary (a handful of PT entries), performance is
not a concern.

```c
/* PTE_PS — Page Size bit. Set in a PDE indicates a 2MB huge page (the entry
 * maps a 2MB frame directly, not a PT). Set in a PDPTE indicates a 1GB page.
 * vmm_map_user_page only creates 4KB PTEs, so no user mapping should ever
 * have PS set. If encountered, skip (leak) rather than misinterpret as a
 * page-table pointer and double-free 512 ghost frames. */
#define PTE_PS (1UL << 7)

void
vmm_free_user_pml4(uint64_t pml4_phys)
{
    int i, j, k, l;
    for (i = 0; i < 256; i++) {
        uint64_t pml4e = ((uint64_t *)vmm_window_map(pml4_phys))[i];
        vmm_window_unmap();
        if (!(pml4e & VMM_FLAG_PRESENT)) continue;
        uint64_t pdpt_phys = pml4e & ~0xFFFUL;

        for (j = 0; j < 512; j++) {
            uint64_t pdpte = ((uint64_t *)vmm_window_map(pdpt_phys))[j];
            vmm_window_unmap();
            if (!(pdpte & VMM_FLAG_PRESENT)) continue;
            if (pdpte & PTE_PS) continue; /* 1GB page — unexpected, skip */
            uint64_t pd_phys = pdpte & ~0xFFFUL;

            for (k = 0; k < 512; k++) {
                uint64_t pde = ((uint64_t *)vmm_window_map(pd_phys))[k];
                vmm_window_unmap();
                if (!(pde & VMM_FLAG_PRESENT)) continue;
                if (pde & PTE_PS) continue; /* 2MB page — unexpected, skip */
                uint64_t pt_phys = pde & ~0xFFFUL;

                for (l = 0; l < 512; l++) {
                    uint64_t pte = ((uint64_t *)vmm_window_map(pt_phys))[l];
                    vmm_window_unmap();
                    if (!(pte & VMM_FLAG_PRESENT)) continue;
                    pmm_free_page(pte & ~0xFFFUL);
                }
                pmm_free_page(pt_phys);
            }
            pmm_free_page(pd_phys);
        }
        pmm_free_page(pdpt_phys);
    }
    pmm_free_page(pml4_phys);
}
```

`PTE_PS` is defined locally in `vmm.c` (not exported to `vmm.h`) — it is an
x86-64 PTE bit and belongs alongside the other architectural PTE manipulation
code in that file.

`vmm_window_map` and `vmm_window_unmap` are static helpers already in `vmm.c`
(used by `vmm_phys_of` and related functions). No new window infrastructure
needed.

### VFS: New file `kernel/fs/vfs.h`

```c
#ifndef VFS_H
#define VFS_H

#include <stdint.h>

#define PROC_MAX_FDS 8

/* File operations vtable. Each open file carries a pointer to its driver's ops. */
typedef struct {
    /* read — copy up to len bytes starting at off into buf (kernel buffer).
     * Returns bytes copied (0 = EOF, negative = error). */
    int (*read)(void *priv, void *buf, uint64_t off, uint64_t len);
    /* close — release any driver-side resources for this file. */
    void (*close)(void *priv);
} vfs_ops_t;

/* Open file descriptor. Embedded inline in aegis_process_t.fds[].
 * ops == NULL means the slot is free. */
typedef struct {
    const vfs_ops_t *ops;    /* NULL = free slot */
    void            *priv;   /* driver-private data */
    uint64_t         offset; /* current read position */
} vfs_file_t;

/* vfs_init — print [VFS] OK line and register built-in drivers.
 * Called from kernel_main before sched_init. */
void vfs_init(void);

/* vfs_open — find a file by path across all registered drivers.
 * Populates *out on success; returns 0 on success, -2 (ENOENT) if not found.
 * Called by sys_open to resolve path to a vfs_file_t. */
int vfs_open(const char *path, vfs_file_t *out);

#endif /* VFS_H */
```

### VFS: New file `kernel/fs/vfs.c`

```c
#include "vfs.h"
#include "initrd.h"
#include "printk.h"

void
vfs_init(void)
{
    printk("[VFS] OK: initialized\n");
    initrd_register();
}

int
vfs_open(const char *path, vfs_file_t *out)
{
    return initrd_open(path, out);
}
```

`vfs_open` dispatches to `initrd_open` directly. When a second filesystem driver
is added, `vfs_open` will try each in order.

### VFS: New file `kernel/fs/initrd.h`

```c
#ifndef INITRD_H
#define INITRD_H

#include "vfs.h"

/* initrd_register — print [INITRD] OK line.
 * Called from vfs_init. */
void initrd_register(void);

/* initrd_open — populate *out if path matches a known file.
 * Returns 0 on success, -2 (ENOENT) if not found. */
int initrd_open(const char *path, vfs_file_t *out);

#endif /* INITRD_H */
```

### VFS: New file `kernel/fs/initrd.c`

```c
#include "initrd.h"
#include "printk.h"
#include <stdint.h>

/* /etc/motd content — starts with '[' so it survives the make test ANSI filter.
 * The filter keeps only lines starting with '['; content not matching is
 * silently dropped from the serial diff. */
static const char s_motd[] = "[MOTD] Hello from initrd!\n";

typedef struct {
    const char *name;
    const char *data;
    uint32_t    size;
} initrd_entry_t;

static const initrd_entry_t s_files[] = {
    { "/etc/motd", s_motd, sizeof(s_motd) - 1 },
    { (const char *)0, (const char *)0, 0 }  /* sentinel */
};

/* s_nfiles used in initrd_register's printk call below. */
static const uint32_t s_nfiles = 1;

static int
initrd_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
    const initrd_entry_t *e = (const initrd_entry_t *)priv;
    if (off >= e->size) return 0;
    uint64_t avail = e->size - off;
    if (len > avail) len = avail;
    __builtin_memcpy(buf, e->data + off, len);
    return (int)len;
}

static void
initrd_close_fn(void *priv)
{
    (void)priv; /* static data, nothing to free */
}

static const vfs_ops_t initrd_ops = {
    .read  = initrd_read_fn,
    .close = initrd_close_fn,
};

void
initrd_register(void)
{
    printk("[INITRD] OK: %u file registered\n", s_nfiles);
}

int
initrd_open(const char *path, vfs_file_t *out)
{
    uint32_t i;
    for (i = 0; s_files[i].name != (const char *)0; i++) {
        /* manual strcmp — no libc in kernel */
        const char *a = path, *b = s_files[i].name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == *b) {
            out->ops    = &initrd_ops;
            out->priv   = (void *)&s_files[i];
            out->offset = 0;
            return 0;
        }
    }
    return -2; /* ENOENT */
}
```

**Why `[MOTD]` prefix:** The `make test` harness strips ANSI sequences and then
filters to lines starting with `[`. Content without a `[` prefix is silently
dropped from the serial diff. The motd content starts with `[MOTD]` so it appears
in the oracle and is matched by the filter.

### Changes to `kernel/proc/proc.h`

Add `#include "vfs.h"` after `#include "sched.h"`.

Add the fd table to `aegis_process_t`:

```c
typedef struct {
    aegis_task_t task;      /* MUST be first */
    uint64_t     pml4_phys;
    vfs_file_t   fds[PROC_MAX_FDS]; /* ops==NULL means slot free */
} aegis_process_t;
```

`PROC_MAX_FDS` is defined in `vfs.h`. `sizeof(vfs_file_t)` = 24 bytes;
8 slots × 24 = 192 bytes. Total `aegis_process_t` fits comfortably in the 4 KB
KVA page already allocated for the PCB.

### Changes to `kernel/proc/proc.c`

In `proc_spawn`, after zeroing/initialising the PCB fields, add:

```c
proc->task.stack_pages = 1;

/* Zero the fd table — all slots start as free (ops == NULL). */
uint64_t i;
for (i = 0; i < PROC_MAX_FDS; i++)
    proc->fds[i].ops = (const vfs_ops_t *)0;
```

`proc.c` already includes `proc.h`, which now includes `vfs.h`. No additional
include needed.

### Changes to `kernel/syscall/syscall.c`

Add includes:
```c
#include "proc.h"
#include "vfs.h"
```

`proc.h` pulls in `sched.h` (for `sched_current`) and `vfs.h` (for `vfs_file_t`
and `PROC_MAX_FDS`). `syscall.c` uses `sched_current()` to get the current
process; since all three new syscalls are reachable only via the `SYSCALL`
instruction from user space, `sched_current()->is_user` is always 1 and the cast
to `aegis_process_t *` is safe.

**`sys_open` (syscall 2):**

```c
static uint64_t
sys_open(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    (void)arg2; (void)arg3;   /* flags and mode ignored in Phase 10 */
    if (!user_ptr_valid(arg1, 256))
        return (uint64_t)-14;  /* EFAULT */
    char kpath[256];
    copy_from_user(kpath, (const void *)(uintptr_t)arg1, 256);
    kpath[255] = '\0';

    aegis_process_t *proc = (aegis_process_t *)sched_current();
    uint64_t fd;
    for (fd = 0; fd < PROC_MAX_FDS; fd++)
        if (!proc->fds[fd].ops) break;
    if (fd == PROC_MAX_FDS)
        return (uint64_t)-24;  /* EMFILE */

    int r = vfs_open(kpath, &proc->fds[fd]);
    if (r < 0)
        return (uint64_t)(int64_t)r;
    return fd;
}
```

**`sys_read` (syscall 0):**

```c
static uint64_t
sys_read(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    if (arg1 >= PROC_MAX_FDS)
        return (uint64_t)-9;   /* EBADF */
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    vfs_file_t *f = &proc->fds[arg1];
    if (!f->ops)
        return (uint64_t)-9;   /* EBADF */
    if (!user_ptr_valid(arg2, arg3))
        return (uint64_t)-14;  /* EFAULT */

    char kbuf[256];
    uint64_t total = 0;
    while (total < arg3) {
        uint64_t n = arg3 - total;
        if (n > sizeof(kbuf)) n = sizeof(kbuf);
        int got = f->ops->read(f->priv, kbuf, f->offset, n);
        if (got <= 0) break;
        copy_to_user((void *)(uintptr_t)(arg2 + total), kbuf, (uint64_t)got);
        f->offset += (uint64_t)got;
        total     += (uint64_t)got;
    }
    return total;
}
```

**`sys_close` (syscall 3):**

```c
static uint64_t
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
```

Add cases to `syscall_dispatch`:
```c
case 0:  return sys_read(arg1, arg2, arg3);
case 2:  return sys_open(arg1, arg2, arg3);
case 3:  return sys_close(arg1);
```

### Changes to `user/init/main.c`

Replace the current `_start` body. Add `sys_open`, `sys_read`, `sys_close`
inline syscall helpers alongside the existing `sys_write` and `sys_exit`:

```c
static inline long
sys_open(const char *path, long flags, long mode)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(2L), "D"((long)path), "S"(flags), "d"(mode)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long
sys_read(long fd, char *buf, long len)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(0L), "D"(fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long
sys_close(long fd)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(3L), "D"(fd)
        : "rcx", "r11", "memory");
    return ret;
}

void
_start(void)
{
    char buf[64];
    long fd = sys_open("/etc/motd", 0, 0);
    long n  = sys_read(fd, buf, (long)sizeof(buf));
    sys_write(1, buf, n);
    sys_close(fd);
    sys_exit(0);
}
```

### Changes to `kernel/core/main.c`

1. Add `#include "vfs.h"` (or `../fs/vfs.h` if needed — build system uses
   `-Ikernel/fs` so `#include "vfs.h"` resolves).
2. Call `vfs_init()` after `arch_smap_init()` and before `sched_init()`.
3. Rename `task_heartbeat` to `task_idle`; replace its body as specified above.
4. Change `sched_spawn(task_heartbeat)` to `sched_spawn(task_idle)`.

Insertion point in `kernel_main`:
```c
arch_smap_init();
vfs_init();         /* [VFS] OK + [INITRD] OK */
sched_init();
```

### Changes to `tests/expected/boot.txt`

Add two new lines after `[SMAP]` and before `[VMM] OK: identity map removed`.
Replace the four `[USER]` lines with one `[MOTD]` line. Remove `[AEGIS]` (it
now comes from `sched_exit`, not from a task — the content and position are
unchanged):

```
[SERIAL] OK: COM1 initialized at 115200 baud
[VGA] OK: text mode 80x25
[PMM] OK: 127MB usable across 2 regions
[VMM] OK: kernel mapped to 0xFFFFFFFF80000000
[VMM] OK: mapped-window allocator active
[KVA] OK: kernel virtual allocator active
[CAP] OK: capability subsystem reserved
[IDT] OK: 48 vectors installed
[PIC] OK: IRQ0-15 remapped to vectors 0x20-0x2F
[PIT] OK: timer at 100 Hz
[KBD] OK: PS/2 keyboard ready
[GDT] OK: ring 3 descriptors installed
[TSS] OK: RSP0 initialized
[SYSCALL] OK: SYSCALL/SYSRET enabled
[SMAP] OK: supervisor access prevention active
[VFS] OK: initialized
[INITRD] OK: 1 file registered
[VMM] OK: identity map removed
[SCHED] OK: scheduler started, 3 tasks
[MOTD] Hello from initrd!
[AEGIS] System halted.
```

**`[SCHED] OK: scheduler started, 3 tasks`** — still 3 tasks: `task_kbd`,
`task_idle`, user init. ✓

### Changes to `Makefile`

Add `-Ikernel/fs` to `CFLAGS`:
```makefile
-Ikernel/fs \
```

Add `FS_SRCS` variable and wire into `ALL_OBJS`:
```makefile
FS_SRCS = \
    kernel/fs/vfs.c \
    kernel/fs/initrd.c

FS_OBJS = $(patsubst kernel/%.c,$(BUILD)/%.o,$(FS_SRCS))

ALL_OBJS = $(BOOT_OBJ) $(ARCH_OBJS) $(ARCH_ASM_OBJS) $(CORE_OBJS) \
           $(MM_OBJS) $(SCHED_OBJS) $(FS_OBJS) $(USERSPACE_OBJS)
```

The generic `$(BUILD)/%.o: kernel/%.c` rule already handles new subdirectories
via `@mkdir -p $(dir $@)`. No new pattern rules needed.

---

## Files Changed

| File | Change |
|------|--------|
| `kernel/sched/sched.h` | Add `stack_pages` field to `aegis_task_t`; add `sched_current()` declaration |
| `kernel/sched/sched.c` | Populate `stack_pages` in `sched_spawn`; use `dying->stack_pages` in `sched_exit`; add `vmm_free_user_pml4` call + shutdown detection; add `sched_current()` |
| `kernel/proc/proc.h` | Add `#include "vfs.h"`; add `fds[PROC_MAX_FDS]` to `aegis_process_t` |
| `kernel/proc/proc.c` | Set `stack_pages = 1`; zero-init fd table in `proc_spawn` |
| `kernel/mm/uaccess.h` | Add `copy_to_user` static inline |
| `kernel/mm/vmm.h` | Declare `vmm_free_user_pml4` |
| `kernel/mm/vmm.c` | Implement `vmm_free_user_pml4` |
| `kernel/fs/vfs.h` | New — types + `vfs_init` + `vfs_open` |
| `kernel/fs/vfs.c` | New — `vfs_init` + `vfs_open` |
| `kernel/fs/initrd.h` | New — `initrd_register` + `initrd_open` |
| `kernel/fs/initrd.c` | New — static file table + ops + read/close |
| `kernel/syscall/syscall.c` | Add `#include "proc.h"`, `#include "vfs.h"`; add `sys_open`, `sys_read`, `sys_close`; add cases to `syscall_dispatch` |
| `kernel/core/main.c` | Add `vfs_init()` call; rename `task_heartbeat` → `task_idle`; rewrite `task_idle` body |
| `user/init/main.c` | Add `sys_open`, `sys_read`, `sys_close` helpers; rewrite `_start` |
| `tests/expected/boot.txt` | Add `[VFS]` and `[INITRD]` lines; replace `[USER]` lines with `[MOTD]` line |
| `Makefile` | Add `-Ikernel/fs`; add `FS_SRCS`/`FS_OBJS`; wire into `ALL_OBJS` |
| `.claude/CLAUDE.md` | Update build status table; update Phase 10 constraints |

---

## Test Oracle

`tests/expected/boot.txt` is updated as shown above. `make test` exits 0 on exact
match. Three audits confirm correctness:

```bash
# Verify copy_to_user added to uaccess.h
grep -n 'copy_to_user' kernel/mm/uaccess.h

# Verify USER_ADDR_MAX still only in syscall_util.h (not regressed)
grep -rn 'USER_ADDR_MAX' kernel/

# Verify stack_pages field used in sched_exit (not is_user inference)
grep -n 'is_user.*STACK_PAGES\|STACK_PAGES.*is_user' kernel/sched/sched.c  # must be empty
```

---

## Success Criteria

1. `make test` exits 0.
2. `grep -rn 'USER_ADDR_MAX' kernel/` shows definition only in `syscall_util.h`.
3. `grep -n 'is_user.*STACK_PAGES' kernel/sched/sched.c` produces no output.
4. `grep -n 'copy_to_user' kernel/mm/uaccess.h` shows the new function.
5. `kernel/fs/` directory exists with `vfs.h`, `vfs.c`, `initrd.h`, `initrd.c`.

---

## Phase 11 Forward-Looking Constraints

**Last-exit leak persists (reduced).** After `vmm_free_user_pml4` frees the user
address space, `sched_exit` records `dying` (the PCB/TCB) in `g_prev_dying_tcb`.
If no subsequent `sched_exit` fires (neither `task_kbd` nor `task_idle` ever
exit), init's TCB (4 KB) leaks at shutdown. Since the kernel exits immediately
after, this is acceptable for Phase 10. A full fix requires either (a) freeing
dying's TCB directly in `sched_exit` before `ctx_switch` — impossible because
`ctx_switch` writes `dying->rsp` — or (b) a reaper task that calls `sched_exit`.

**ELF KVA dangling PTEs.** The KVA PTEs for ELF staging pages remain after
`vmm_free_user_pml4` frees the physical frames. Safe for Phase 10 (those VAs are
never accessed again, bump allocator never rewinds). A proper fix requires the ELF
loader to call `kva_free_pages` on staging VAs after mapping into the user PML4.
This requires the ELF loader to retain the KVA VA, which means threading it
through the load loop. Deferred to when the ELF loader is refactored for
multi-segment support.

**`vmm_free_user_pml4` is single-CPU.** Uses local TLB invalidation only (via
`vmm_window_map`/`vmm_window_unmap`). SMP requires a TLB shootdown IPI to all
other CPUs before any freed physical page is reused.

**`sys_open` path length.** `user_ptr_valid(arg1, 256)` requires the path buffer
to be at least 256 bytes long in user space. A path shorter than 256 bytes that
sits near a page boundary where the next page is unmapped will trigger EFAULT even
though the actual string is shorter. A proper fix scans for the NUL terminator
byte by byte (or page by page). Deferred to when paths longer than a few bytes
exist.

**No `stdin` (fd 0).** `sys_read` from fd 0 returns EBADF — fd 0 is never
populated. Phase 11+ VFS should pre-wire fd 0 to a keyboard driver once
`kbd_read` is wrapped in a `vfs_ops_t`.

**VFS has no mount table.** `vfs_open` calls `initrd_open` directly. Adding a
second filesystem requires extending `vfs_open` to iterate a registered-driver
table. The `vfs_ops_t` vtable at the file level is correct; the filesystem
registration layer is the missing piece.
