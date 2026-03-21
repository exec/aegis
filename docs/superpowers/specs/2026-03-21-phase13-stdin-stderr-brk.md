# Phase 13: stdin + stderr + sys_brk

## Goal

Three related items that complete the standard I/O model and add user-space heap
allocation — the last prerequisites before a musl port is feasible:

1. **Keyboard VFS driver** — wraps `kbd_read()` in a `vfs_ops_t`; pre-opened as
   fd 0 (stdin) at process spawn.
2. **stderr pre-open** — fd 2 pre-opened to the console device at spawn; completes
   the standard fd 0/1/2 triple.
3. **`sys_brk`** — user-space heap via a moving break pointer; `aegis_process_t`
   gains a `brk` field; `elf_load` gains an out-param for the heap start address.
4. **`CAP_KIND_VFS_READ` gate on `sys_read`** — Phase 11 oversight: `sys_read` has
   no capability check. Phase 13 adds it, consistent with `sys_open`/`sys_write`.
5. **Remove `task_kbd`** — the kernel echo task competed with user-space stdin;
   now that fd 0 is owned by user space, `task_kbd` is retired. Task count drops
   from 3 to 2.

---

## Prerequisites

Phase 12 must be fully implemented and `make test` must be GREEN. Verify:

```bash
grep -n 'CAP_KIND_VFS_WRITE' kernel/cap/cap.h   # must print a line
grep -n 'console_open'       kernel/fs/console.h # must print a line
make test                                         # must exit 0
```

---

## Background

### Why remove `task_kbd`

`task_kbd` was introduced in Phase 4 as a keyboard-echo demo. It calls `kbd_read()`
in a blocking loop. Once user space pre-opens fd 0 to the keyboard VFS driver,
both `task_kbd` and the user process compete for the same `kbd_read()` call — the
kernel ring buffer delivers each keystroke to whichever consumer calls `kbd_read()`
first. This is a race condition. The correct resolution is to retire `task_kbd`
entirely: user space owns stdin from Phase 13 onward.

Removing `task_kbd` reduces the run-queue to 2 tasks at `sched_start`: `task_idle`
and the init user process. The `[SCHED]` oracle line changes from `3 tasks` to
`2 tasks`.

### Why `sys_brk` does not need a capability

`sys_brk` only expands or contracts the calling process's own address space. The
process already holds authority over its address space by virtue of having one —
there is no shared resource being accessed. Capability gates apply at resource
boundaries (files, devices, inter-process communication). A process requesting
more of its own heap does not cross a resource boundary. No `CAP_KIND_*` constant
is introduced for `sys_brk`.

### Why no buddy allocator is needed

`sys_brk` allocates pages one at a time via `pmm_alloc_page()` +
`vmm_map_user_page()`. The PMM's single-page allocator is sufficient because `brk`
grows linearly and each increment is one page. The buddy allocator is a kernel
concern (large contiguous kernel allocations); it is not required for user heap.

### Why `elf_load` needs an out-param

`proc_spawn` currently discards the top-of-segments VA after ELF loading. Placing
the heap immediately above the ELF avoids the fragility of a hardcoded
`USER_BRK_START` constant that would break if the ELF grows. The ELF loader
already iterates all `PT_LOAD` segments — tracking the highest mapped VA costs one
extra comparison per segment.

### Why `sys_read` needs a capability gate

`sys_open` requires `CAP_KIND_VFS_OPEN`. `sys_write` requires `CAP_KIND_VFS_WRITE`.
`sys_read` has no gate — a Phase 11 oversight. Any process that holds a valid fd
can read from it without a matching capability. `CAP_KIND_VFS_READ (3u)` corrects
this: `sys_read` checks for it before accessing the fd table. `proc_spawn` grants
it alongside the other two capabilities.

---

## Architecture

### New file: `kernel/fs/kbd_vfs.h`

```c
#ifndef KBD_VFS_H
#define KBD_VFS_H

#include "vfs.h"

/* kbd_vfs_open — return a pointer to the static keyboard vfs_file_t.
 * Used by proc_spawn to pre-populate fd 0 (stdin). */
vfs_file_t *kbd_vfs_open(void);

#endif /* KBD_VFS_H */
```

No `kbd_vfs_init()` — the keyboard driver is already initialised by `kbd_init()`
in `kernel_main`. The VFS wrapper requires no separate init step.

### New file: `kernel/fs/kbd_vfs.c`

```c
#include "kbd_vfs.h"
#include "vfs.h"
#include "kbd.h"       /* kbd_read() */
#include "uaccess.h"   /* copy_to_user() */
#include <stdint.h>

static int
kbd_vfs_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
    (void)priv; (void)off;  /* stream device — offset is ignored */
    uint64_t i;
    char *kbuf = (char *)buf;
    for (i = 0; i < len; i++)
        kbuf[i] = kbd_read();   /* blocks until key available */
    return (int)len;
}

static int
kbd_vfs_write_fn(void *priv, const void *buf, uint64_t len)
{
    (void)priv; (void)buf; (void)len;
    return -38; /* ENOSYS — stdin is not writable */
}

static void
kbd_vfs_close_fn(void *priv)
{
    (void)priv; /* stateless — nothing to release */
}

static const vfs_ops_t s_kbd_ops = {
    .read  = kbd_vfs_read_fn,
    .write = kbd_vfs_write_fn,
    .close = kbd_vfs_close_fn,
};

static vfs_file_t s_kbd_file = {
    .ops    = &s_kbd_ops,
    .priv   = (void *)0,
    .offset = 0,
};

vfs_file_t *
kbd_vfs_open(void)
{
    return &s_kbd_file;
}
```

**Design note:** `kbd_vfs_read_fn` receives a kernel buffer `buf` that comes from
`sys_read`'s stack. `sys_read` copies the result to user space via `copy_to_user`
after the driver returns — the VFS driver itself never touches user memory directly.
This matches the existing `initrd_read_fn` pattern.

**`write` stub:** `vfs_ops_t` now has a `write` field (added in Phase 12). The
keyboard driver sets it to `kbd_vfs_write_fn` returning `ENOSYS` rather than
leaving it NULL. This allows `sys_write` to give the correct `ENOSYS` error rather
than `EBADF` when a program mistakenly writes to stdin.

### Changes to `kernel/cap/cap.h`

Add after `CAP_KIND_VFS_WRITE`:

```c
#define CAP_KIND_VFS_READ  3u   /* permission to call sys_read */
```

### Changes to `kernel/elf/elf.h`

Change the `elf_load` signature to add an out-param for the heap start:

```c
/* elf_load — parse ELF, map PT_LOAD segments into pml4_phys.
 * Returns entry RIP on success, 0 on parse error.
 * *out_brk is set to the first page-aligned VA above all loaded segments.
 * Used by proc_spawn to initialise proc->brk. */
uint64_t elf_load(uint64_t pml4_phys, const uint8_t *data,
                  size_t len, uint64_t *out_brk);
```

### Changes to `kernel/elf/elf.c`

Read the file first to understand current structure. The ELF loader iterates
`PT_LOAD` segments. Add a local `uint64_t seg_end = 0` before the loop. In the
loop body, after computing `vaddr + filesz` (or `vaddr + memsz`) for each loaded
segment, update:

```c
uint64_t this_end = phdr->p_vaddr + phdr->p_memsz;
if (this_end > seg_end)
    seg_end = this_end;
```

After the loop, write the page-aligned result to `out_brk`:

```c
*out_brk = (seg_end + 4095UL) & ~4095UL;  /* round up to page boundary */
```

The call site in `proc_spawn` changes to:

```c
uint64_t brk_start;
uint64_t entry_rip = elf_load(proc->pml4_phys, elf_data, elf_len, &brk_start);
if (!entry_rip) { /* error path unchanged */ }
```

### Changes to `kernel/proc/proc.h`

Add `#include "kbd_vfs.h"` and a `brk` field to `aegis_process_t`:

```c
#include "kbd_vfs.h"

typedef struct {
    aegis_task_t  task;
    uint64_t      pml4_phys;
    vfs_file_t    fds[PROC_MAX_FDS];
    cap_slot_t    caps[CAP_TABLE_SIZE];
    uint64_t      brk;    /* current heap limit (user VA); grows up */
} aegis_process_t;
```

### Changes to `kernel/proc/proc.c`

After the existing two `cap_grant` calls and before `sched_add`, add:

```c
/* Grant read capability. */
if (cap_grant(proc->caps, CAP_TABLE_SIZE,
              CAP_KIND_VFS_READ, CAP_RIGHTS_READ) < 0) {
    printk("[CAP] FAIL: cap_grant VFS_READ returned -ENOCAP\n");
    for (;;) {}
}

/* Pre-open fd 0 (stdin) to keyboard device. */
proc->fds[0] = *kbd_vfs_open();

/* fd 1 (stdout) already pre-opened to console in Phase 12. */

/* Pre-open fd 2 (stderr) to console device. */
proc->fds[2] = *console_open();

/* Initialise heap break to top of ELF segments. */
proc->brk = brk_start;   /* set from elf_load out-param */

printk("[CAP] OK: 3 capabilities granted to init\n");
```

Replace the existing `[CAP] OK: 2 capabilities granted to init\n` printk with
the new `3 capabilities` line. The `sched_add` call remains last.

Also add `#include "kbd_vfs.h"` to the includes in `proc.c`.

**Note on fd 0 conflict with `elf_load`'s `out_brk` param:** `proc.c` must declare
`brk_start` before calling `elf_load`. Read the current `proc_spawn` function
carefully and update the `elf_load` call site to pass `&brk_start`.

### Changes to `kernel/syscall/syscall.c`

**Add `cap_check(VFS_READ)` gate at the top of `sys_read`:**

```c
static uint64_t
sys_read(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_READ, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    /* ... existing fd validation and read loop unchanged ... */
}
```

**Add `sys_brk` (syscall 12):**

```c
/*
 * sys_brk — syscall 12
 *
 * arg1 = requested new break address (0 = query current brk)
 *
 * Returns the new (or current) break address.
 * On OOM, returns the current break unchanged (Linux-compatible).
 * No capability gate — process expands its own address space only.
 */
static uint64_t
sys_brk(uint64_t arg1)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();

    if (arg1 == 0)
        return proc->brk;  /* query */

    /* Clamp to user address space — must not reach kernel half. */
    if (arg1 >= 0x00007FFFFFFFFFFFULL)
        return proc->brk;

    if (arg1 > proc->brk) {
        /* Grow: allocate pages for [proc->brk, arg1). */
        uint64_t va;
        for (va = proc->brk; va < arg1; va += 4096UL) {
            uint64_t phys = pmm_alloc_page();
            if (!phys)
                return proc->brk;  /* OOM — return current brk */
            vmm_map_user_page(proc->pml4_phys, va, phys,
                              VMM_FLAG_PRESENT | VMM_FLAG_USER |
                              VMM_FLAG_WRITABLE);
        }
        proc->brk = arg1;
    } else if (arg1 < proc->brk) {
        /* Shrink: free pages for [arg1, proc->brk). */
        uint64_t va;
        for (va = arg1; va < proc->brk; va += 4096UL) {
            uint64_t phys = vmm_phys_of_user(proc->pml4_phys, va);
            if (phys) {
                vmm_unmap_user_page(proc->pml4_phys, va);
                pmm_free_page(phys);
            }
        }
        proc->brk = arg1;
    }

    return proc->brk;
}
```

Add `case 12: return sys_brk(arg1);` to `syscall_dispatch`.

**New VMM helpers required by `sys_brk` shrink path:**

`sys_brk` must walk a *specific process's* page table (not the active one) to
translate a user VA to physical and to unmap it. Two new functions are needed in
`vmm.h` / `vmm.c`:

```c
/* vmm_phys_of_user — walk pml4_phys to find the physical address mapped at virt.
 * Returns physical address, or 0 if not mapped.
 * Uses the window allocator. Safe to call with any PML4. */
uint64_t vmm_phys_of_user(uint64_t pml4_phys, uint64_t virt);

/* vmm_unmap_user_page — clear the PTE for virt in pml4_phys and invlpg.
 * Does not free the physical page. Caller frees it via pmm_free_page. */
void vmm_unmap_user_page(uint64_t pml4_phys, uint64_t virt);
```

`vmm_phys_of_user` follows the same walk-overwrite window pattern as the existing
`vmm_phys_of` (which walks the *active* PML4). The difference is that it starts
from an explicit `pml4_phys` instead of `s_pml4_phys`.

`vmm_unmap_user_page` walks to the PT level, zeroes the PTE, and calls
`arch_vmm_invlpg`. It is analogous to the existing `vmm_unmap_page` but operates
on an arbitrary PML4.

**Note on `sys_brk` grow path:** The grow loop must map pages at `proc->pml4_phys`,
not the active CR3 (which is the master PML4 during syscall entry). Use
`vmm_map_user_page(proc->pml4_phys, va, phys, flags)` — this already takes an
explicit `pml4_phys` and uses the window allocator internally. No change to
`vmm_map_user_page` needed.

**Note on arg1 alignment:** `sys_brk` does not require page alignment from the
caller. If `arg1` is not page-aligned, the grow/shrink loops will map/unmap partial
pages correctly because the loop increments by `4096` from the *current brk*, which
is always page-aligned. However, `proc->brk` should be stored as the exact value
the caller requested (Linux-compatible). Consider rounding `arg1` up to page
boundary on grow (so the mapping covers the requested range) and down on shrink.
The simplest correct implementation: always page-align `arg1` upward on both grow
and shrink before the loop, store the aligned value in `proc->brk`.

Add includes to `syscall.c`: `"vmm.h"` and `"pmm.h"` (for `vmm_map_user_page`,
`vmm_phys_of_user`, `vmm_unmap_user_page`, `pmm_alloc_page`, `pmm_free_page`).
Read the current includes to check which are already present.

### Changes to `kernel/core/main.c`

Remove `task_kbd` function and its `sched_spawn(task_kbd)` call. The `task_idle`
kernel task remains. `sched_spawn` is called once: `sched_spawn(task_idle)`.

No new init call needed for the keyboard VFS — `kbd_init()` (already in main.c)
initialises the hardware driver; the VFS wrapper is stateless.

### Changes to `Makefile`

Add `kernel/fs/kbd_vfs.c` to `FS_SRCS`:

```makefile
FS_SRCS = \
    kernel/fs/vfs.c \
    kernel/fs/initrd.c \
    kernel/fs/console.c \
    kernel/fs/kbd_vfs.c
```

### Changes to `user/init/main.c`

Add a `sys_brk` inline syscall helper and update `_start` to test the heap:

```c
static inline long
sys_brk(long addr)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(12L), "D"(addr)
        : "rcx", "r11", "memory");
    return ret;
}

void
_start(void)
{
    /* Read and print motd (tests VFS + sys_read + sys_write). */
    char buf[64];
    long fd = sys_open("/etc/motd", 0, 0);
    long n  = sys_read(fd, buf, (long)sizeof(buf));
    sys_write(1, buf, n);
    sys_close(fd);

    /* Test sys_brk: allocate one heap page, write a string, print it. */
    long heap = sys_brk(0);               /* query current brk */
    sys_brk(heap + 4096);                 /* allocate one page */
    char *p = (char *)heap;
    /* Write "[HEAP] OK: brk works\n" into heap memory byte-by-byte
     * (no memcpy/strcpy — freestanding, no libc). */
    const char msg[] = "[HEAP] OK: brk works\n";
    int i;
    for (i = 0; msg[i]; i++)
        p[i] = msg[i];
    sys_write(1, p, i);

    sys_exit(0);
}
```

**Why `[HEAP]` prefix:** the `make test` harness keeps only lines starting with
`[`. The message starts with `[HEAP]` so it appears in the oracle diff.

### Changes to `tests/expected/boot.txt`

Three changes from the Phase 12 oracle:
1. `3 capabilities` (was `2 capabilities`)
2. `2 tasks` (was `3 tasks` — `task_kbd` removed)
3. New line `[HEAP] OK: brk works` after `[MOTD]`

```
[SERIAL] OK: COM1 initialized at 115200 baud
[VGA] OK: text mode 80x25
[PMM] OK: 127MB usable across 2 regions
[VMM] OK: kernel mapped to 0xFFFFFFFF80000000
[VMM] OK: mapped-window allocator active
[KVA] OK: kernel virtual allocator active
[CAP] OK: capability subsystem initialized
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
[CAP] OK: 3 capabilities granted to init
[VMM] OK: identity map removed
[SCHED] OK: scheduler started, 2 tasks
[MOTD] Hello from initrd!
[HEAP] OK: brk works
[AEGIS] System halted.
```

---

## Files Changed

| File | Change |
|------|--------|
| `kernel/fs/kbd_vfs.h` | New — kbd VFS interface |
| `kernel/fs/kbd_vfs.c` | New — `kbd_vfs_read_fn` + stateless singleton |
| `kernel/cap/cap.h` | Add `CAP_KIND_VFS_READ 3u` |
| `kernel/elf/elf.h` | Add `uint64_t *out_brk` out-param to `elf_load` |
| `kernel/elf/elf.c` | Track highest mapped VA; write to `out_brk` |
| `kernel/mm/vmm.h` | Declare `vmm_phys_of_user` + `vmm_unmap_user_page` |
| `kernel/mm/vmm.c` | Implement `vmm_phys_of_user` + `vmm_unmap_user_page` |
| `kernel/proc/proc.h` | Add `#include "kbd_vfs.h"`; add `brk` field to `aegis_process_t` |
| `kernel/proc/proc.c` | Add VFS_READ grant; pre-open fd 0 + fd 2; set `proc->brk`; update printk |
| `kernel/syscall/syscall.c` | Add `cap_check(VFS_READ)` to `sys_read`; add `sys_brk` (syscall 12) |
| `kernel/core/main.c` | Remove `task_kbd` + `sched_spawn(task_kbd)` |
| `Makefile` | Add `kernel/fs/kbd_vfs.c` to `FS_SRCS` |
| `user/init/main.c` | Add `sys_brk` helper; test heap alloc in `_start` |
| `tests/expected/boot.txt` | 3 capabilities; 2 tasks; add `[HEAP]` line |
| `.claude/CLAUDE.md` | Update build status |

---

## Test Oracle

`make test` exits 0. Verification audits:

```bash
# kbd VFS driver exists
ls kernel/fs/kbd_vfs.c kernel/fs/kbd_vfs.h

# CAP_KIND_VFS_READ defined
grep -n 'CAP_KIND_VFS_READ' kernel/cap/cap.h

# sys_read has cap_check gate
grep -n 'cap_check' kernel/syscall/syscall.c | grep sys_read

# sys_brk registered in dispatch
grep -n 'sys_brk' kernel/syscall/syscall.c

# task_kbd removed from main.c
grep -n 'task_kbd' kernel/core/main.c  # must produce NO output

# brk field in process struct
grep -n 'brk' kernel/proc/proc.h
```

---

## Success Criteria

1. `make test` exits 0 with the new oracle.
2. `grep -n 'task_kbd' kernel/core/main.c` returns nothing.
3. `grep -n 'CAP_KIND_VFS_READ' kernel/cap/cap.h` shows the definition.
4. `grep -n 'cap_check' kernel/syscall/syscall.c` shows gates in both `sys_read`
   and `sys_open` (and `sys_write`).
5. `grep -n 'brk' kernel/proc/proc.h` shows the `brk` field.

---

## Phase 14 Forward-Looking Constraints

**`sys_brk` page-aligns the break.** `proc->brk` is always page-aligned after
a grow or shrink. User-space allocators (musl's `malloc`) typically call `brk`
with exact byte offsets; they expect the kernel to return the actual new break,
which may be rounded up. Phase 13 rounds up to page boundary on grow — musl
handles this correctly. Document this contract in the syscall comment.

**fd 0 blocks on `kbd_read()`.** A user process that calls `sys_read(0, ...)` will
block until a key is pressed. In the headless `make test` environment there is no
keyboard input, so `init` must not call `sys_read(0, ...)` in Phase 13. Phase 14
(or whenever a shell arrives) should provide a `kbd_poll`-based non-blocking path
or use `sys_select`/`sys_poll`.

**`vmm_phys_of_user` walks the target PML4, not the active CR3.** The shrink path
in `sys_brk` must pass `proc->pml4_phys` explicitly — calling `vmm_phys_of` (which
uses `s_pml4_phys`, the active kernel PML4) would be incorrect during a syscall
where CR3 holds the master PML4.

**No `sys_mmap`.** musl's allocator falls back to `mmap(MAP_ANONYMOUS)` if `brk`
fails. Phase 13 provides no `mmap`. A musl port will require either a `brk`-only
allocator configuration (musl supports this) or a minimal `sys_mmap` in Phase 14.

**Capability delegation still deferred.** A second user process must receive
capabilities via `proc_spawn` grants. `sys_cap_grant` for parent→child delegation
remains future work.
