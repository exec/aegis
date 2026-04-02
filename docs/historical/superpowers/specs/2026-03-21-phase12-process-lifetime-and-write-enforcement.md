# Phase 12: Process Lifetime Cleanup + sys_write Enforcement

## Goal

Two independent halves, implemented in order:

**Half 1 — Correctness:**
1. **`vmm_free_user_pml4`** — free user address space on process exit; eliminates the PML4 + ELF segment page leak.
2. **`stack_pages` field** — replace the `is_user ? 1 : STACK_PAGES` inference in `sched_exit` with an explicit per-task field.
3. **Permanent `task_idle`** — rename `task_heartbeat`, remove the 500-tick exit; shutdown detection moves to `sched_exit` via an `is_user` scan.

**Half 2 — sys_write enforcement:**
4. **Console device** (`kernel/fs/console.c`) — VFS driver that routes writes to `printk`.
5. **fd 1 pre-opened at spawn** — `proc_spawn` opens the console device into fd 1; user process inherits stdout with no syscall.
6. **`CAP_KIND_VFS_WRITE`** — new capability kind; `sys_write` capability-gated; routes through the fd table instead of calling `printk` directly.

After Phase 12, `sys_write` is architecturally consistent with `sys_open`: both require a capability, both route through the fd table. A process that holds no `CAP_KIND_VFS_WRITE` cannot produce output.

---

## Prerequisites

Phase 12 depends on Phase 10 (VFS + fd table + `sys_open`/`sys_read`/`sys_close` + `sched_current()`) and Phase 11 (capability system: `cap_grant`, `cap_check`, `CAP_TABLE_SIZE`, `CapSlot`, `caps[]` in `aegis_process_t`). Both must be fully implemented and `make test` must be GREEN before any Phase 12 file is touched.

**HARD STOP:** As of Phase 9 (the current codebase), none of the Phase 10 or 11 infrastructure exists. `kernel/fs/` does not exist, `aegis_process_t` has no `fds[]` or `caps[]` fields, and `cap_grant`/`cap_check` are not implemented. Do not create any Phase 12 file until all four checks below pass:

```bash
grep -n 'sched_current' kernel/sched/sched.h    # must print a declaration
grep -n 'cap_grant' kernel/cap/cap.h             # must print a declaration
grep -n 'vfs_file_t' kernel/fs/vfs.h             # must print the typedef
make test                                         # must exit 0
```

If `sched_current` is missing, request that Phase 10 adds `aegis_task_t *sched_current(void)` to `sched.h` (returning `s_current`) before proceeding. If any other check fails, Phase 10 or 11 is incomplete.

---

## Architecture

### Half 1: Correctness

#### `vmm_free_user_pml4` — `kernel/mm/vmm.c` + `vmm.h`

New function that walks `PML4[0..255]` (the user half; entries 256–511 are the shared kernel half and must never be touched) and frees all mapped user frames and page-table pages.

**Interface:**

```c
/* vmm_free_user_pml4 — free all user-space mappings in a process PML4.
 * Walks PML4[0..255] only. Entries 256–511 are the kernel half (shared
 * with the master PML4) and must not be touched.
 * Frees: all 4KB user frames, all PT/PD/PDPT pages, and the PML4 page itself.
 * Called from sched_exit after switching to master PML4. */
void vmm_free_user_pml4(uint64_t pml4_phys);
```

**Implementation pattern** — per-entry `vmm_window_map`/`vmm_window_unmap` (same as the window-walk pattern established in Phase 6). No bulk local arrays (would risk stack overflow on a 16KB kernel stack with three nesting levels × 512 entries × 8 bytes = 12KB). No walk-overwrite — that pattern is for single-path descents and is documented in CLAUDE.md for `vmm_phys_of`-style uses. It must not be used here because this function iterates all 256 PML4 entries, not a single path. The per-entry map/unmap is correct and required for this iteration pattern.

```c
/* PTE_PS: huge-page flag (bit 7). No VMM_FLAG_* equivalent exists.
 * VMM_FLAG_PRESENT (bit 0) is already defined in vmm.h — use it directly. */
#define PTE_PS (1UL << 7)   /* huge page — skip, don't dereference as table */

void
vmm_free_user_pml4(uint64_t pml4_phys)
{
    uint64_t i, j, k, l;

    for (i = 0; i < 256; i++) {
        uint64_t *pml4e_p = vmm_window_map(pml4_phys);
        uint64_t  pml4e   = pml4e_p[i];
        vmm_window_unmap();
        if (!(pml4e & VMM_FLAG_PRESENT)) continue;

        uint64_t pdpt_phys = pml4e & ~0xFFFUL;
        for (j = 0; j < 512; j++) {
            uint64_t *pdpte_p = vmm_window_map(pdpt_phys);
            uint64_t  pdpte   = pdpte_p[j];
            vmm_window_unmap();
            if (!(pdpte & VMM_FLAG_PRESENT)) continue;
            if (  pdpte & PTE_PS)            continue; /* 1GB page — unexpected, skip */

            uint64_t pd_phys = pdpte & ~0xFFFUL;
            for (k = 0; k < 512; k++) {
                uint64_t *pde_p = vmm_window_map(pd_phys);
                uint64_t  pde   = pde_p[k];
                vmm_window_unmap();
                if (!(pde & VMM_FLAG_PRESENT)) continue;
                if (  pde & PTE_PS)            continue; /* 2MB page — unexpected, skip */

                uint64_t pt_phys = pde & ~0xFFFUL;
                for (l = 0; l < 512; l++) {
                    uint64_t *pte_p = vmm_window_map(pt_phys);
                    uint64_t  pte   = pte_p[l];
                    vmm_window_unmap();
                    if (!(pte & VMM_FLAG_PRESENT)) continue;
                    pmm_free_page(pte & ~0xFFFUL); /* free user frame */
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

**Call site in `sched_exit`** — insert the `vmm_free_user_pml4` call between `dying = s_current` and `s_current = dying->next` in the existing function. Preserve all other existing logic: the deferred cleanup of `g_prev_dying_tcb`, the `arch_set_kernel_stack` call, the CR3-switch policy block (`if (dying->is_user && !s_current->is_user) ...`), and the `g_prev_dying_*` assignment before `ctx_switch`. Do not remove any of these. The `vmm_window_map`/`vmm_window_unmap` helpers are `static` functions in `vmm.c` — `vmm_free_user_pml4` lives in the same file and calls them directly without any declaration change needed.

```c
void
sched_exit(void)
{
    vmm_switch_to(vmm_get_master_pml4());

    /* ... deferred cleanup of previous dying task (g_prev_dying_tcb) ... */

    aegis_task_t *prev = s_current;
    while (prev->next != s_current)
        prev = prev->next;

    aegis_task_t *dying = s_current;

    /* Free user address space now that dying is identified and master PML4
     * is loaded. The walk runs in kernel context; the user PML4 is
     * unreachable from the CPU after vmm_switch_to above. */
    if (dying->is_user)
        vmm_free_user_pml4(((aegis_process_t *)dying)->pml4_phys);

    s_current  = dying->next;
    prev->next = s_current;
    s_task_count--;

    /* ... shutdown scan and ctx_switch ... */
}
```

`VMM_FLAG_PRESENT` is already defined in `vmm.h` and used directly. `PTE_PS` has no existing `VMM_FLAG_*` equivalent and is defined locally in `vmm.c` — not exported. Both are standard x86-64 page-table flag values.

---

#### `stack_pages` field — `kernel/sched/sched.h` + `sched.c` + `proc.c`

Add `uint64_t stack_pages` to `aegis_task_t` after `stack_base`:

```c
typedef struct aegis_task_t {
    uint64_t             rsp;
    uint8_t             *stack_base;
    uint64_t             stack_pages;          /* Phase 12: pages allocated at stack_base */
    uint64_t             kernel_stack_top;
    uint32_t             tid;
    uint8_t              is_user;
    struct aegis_task_t *next;
} aegis_task_t;
```

Write sites:
- `sched_spawn`: `task->stack_pages = STACK_PAGES;`
- `proc_spawn`: `proc->task.stack_pages = 1;`

Read site — `sched_exit` replaces:
```c
/* before */
g_prev_dying_stack_pages = dying->is_user ? 1 : STACK_PAGES;

/* after */
g_prev_dying_stack_pages = dying->stack_pages;
```

No other changes. The `_Static_assert(offsetof(aegis_task_t, rsp) == 0, ...)` in `sched.c` still holds — `rsp` remains first.

---

#### Permanent `task_idle` — `kernel/core/main.c` + `kernel/sched/sched.c`

**In `main.c`:** rename `task_heartbeat` to `task_idle`. Remove the 500-tick counter and `sys_exit` call. Replace the body with:

```c
static void
task_idle(void)
{
    arch_enable_interrupts();
    for (;;)
        __asm__ volatile ("hlt");
}
```

**Shutdown detection in `sched_exit`:** after removing `dying` from the run queue and decrementing `s_task_count`, scan remaining tasks for any with `is_user == 1`. If none remain, the user workload is done:

```c
/* Check whether any user process remains after this exit.
 * At this point s_current == dying->next (the list has already been
 * advanced); the scan therefore covers surviving tasks only and cannot
 * encounter dying again. */
if (dying->is_user) {
    aegis_task_t *t = s_current;
    uint8_t has_user = 0;
    do {
        if (t->is_user) { has_user = 1; break; }
        t = t->next;
    } while (t != s_current);

    if (!has_user) {
        printk("[AEGIS] System halted.\n");
        arch_request_shutdown();
        for (;;) __asm__ volatile ("hlt");
    }
}
```

Insert this block after `s_task_count--` and before the existing `if (s_current == dying)` guard. The existing guard (`if (s_current == dying)`) becomes vestigial with the permanent `task_idle` in place — idle never exits, so `s_current` can never equal `dying` after list manipulation. Retain it anyway as a defensive trap.

The `[AEGIS] System halted.` line moves from wherever it was previously emitted to this location. The `boot.txt` oracle position of that line is unchanged.

---

### Half 2: sys_write Enforcement

#### Console device — `kernel/fs/console.c` + `kernel/fs/console.h`

A minimal VFS driver that routes writes to `printk`. No state, no allocation.

**`kernel/fs/console.h`:**

```c
#ifndef CONSOLE_H
#define CONSOLE_H

#include "vfs.h"

/* console_init — register the console VFS device.
 * Called from kernel_main after vfs_init(). Silent — no boot line. */
void console_init(void);

/* console_open — return a vfs_file_t backed by the console device.
 * Used by proc_spawn to pre-populate fd 1. */
vfs_file_t *console_open(void);

#endif /* CONSOLE_H */
```

**`kernel/fs/console.c`:**

```c
#include "console.h"
#include "vfs.h"
#include "uaccess.h"
#include "printk.h"
#include <stddef.h>

#define CONSOLE_BUF 256

static uint64_t
console_write(vfs_file_t *f, const void *buf, uint64_t len)
{
    (void)f;
    char kbuf[CONSOLE_BUF];
    uint64_t n = (len > CONSOLE_BUF) ? CONSOLE_BUF : len;
    if (copy_from_user(kbuf, buf, n) != 0)
        return (uint64_t)-14; /* EFAULT */
    /* printk one byte at a time — no format string, raw output. */
    uint64_t i;
    for (i = 0; i < n; i++)
        printk("%c", kbuf[i]);
    /* Return actual bytes written, not the requested len.
     * If len > CONSOLE_BUF the caller will see a short write and may retry. */
    return n;
}

static uint64_t
console_read(vfs_file_t *f, void *buf, uint64_t len)
{
    (void)f; (void)buf; (void)len;
    return (uint64_t)-38; /* ENOSYS */
}

static int console_close(vfs_file_t *f) { (void)f; return 0; }

static const vfs_ops_t s_console_ops = {
    .write = console_write,
    .read  = console_read,
    .close = console_close,
};

static vfs_file_t s_console_file = {
    .ops  = &s_console_ops,
    .priv = NULL,
};

void
console_init(void)
{
    /* Nothing to initialize — console is a stateless singleton. */
}

vfs_file_t *
console_open(void)
{
    return &s_console_file;
}
```

**Note on `console_write` buffering:** `CONSOLE_BUF = 256` matches the typical line length for user output. If the user process writes more than 256 bytes in one call, the write is truncated to 256. This is acceptable for Phase 12 — the only user output is the `[MOTD]` line (26 bytes). A more general implementation can increase the buffer or loop when Phase 13 adds a real shell.

**`console_init()` call site** — in `kernel_main`, immediately after `vfs_init()`:

```c
vfs_init();
console_init();   /* Phase 12: register stdout device */
initrd_init();
```

No new boot line. `console_init` is silent.

---

#### `CAP_KIND_VFS_WRITE` — `kernel/cap/cap.h`

Add after `CAP_KIND_VFS_OPEN`:

```c
#define CAP_KIND_VFS_WRITE 2u   /* permission to call sys_write */
```

No changes to `kernel/cap/src/lib.rs` — `cap_grant` and `cap_check` are already generic.

---

#### fd 1 pre-opened + second grant — `kernel/proc/proc.c`

In `proc_spawn`, after the existing `cap_grant` for `CAP_KIND_VFS_OPEN`:

```c
/* Grant write capability. */
if (cap_grant(proc->caps, CAP_TABLE_SIZE,
              CAP_KIND_VFS_WRITE, CAP_RIGHTS_WRITE) >= 0) {
    /* fd 1 already granted above, count includes both */
} else {
    printk("[CAP] FAIL: cap_grant VFS_WRITE returned -ENOCAP\n");
    for (;;) {}
}

/* Pre-open fd 1 (stdout) to the console device.
 * User process inherits stdout without a sys_open call. */
proc->fds[1] = *console_open();

printk("[CAP] OK: 2 capabilities granted to init\n");
```

**Note:** The `printk` moves to after both grants. Replace the existing `[CAP] OK: 1 capability granted to init` printk entirely — do not emit two lines.

The `console_open()` call requires `#include "console.h"` in `proc.c`. Add it alongside the existing VFS include from Phase 10.

---

#### `sys_write` rework — `kernel/syscall/syscall.c`

**Phase 10** implemented `sys_write` to route through the fd table for all fds. Phase 12 adds a capability check at the top and removes the `fd == 1 → printk` special case (which Phase 10 may still have retained as a fallback). The Phase 12 `sys_write` is:

```c
static uint64_t
sys_write(uint64_t fd, uint64_t buf, uint64_t len)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();

    /* Capability gate — must hold VFS_WRITE before touching any fd. */
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_WRITE, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    if (fd >= PROC_MAX_FDS || proc->fds[fd].ops == NULL)
        return (uint64_t)-9; /* EBADF */

    /* Delegate to VFS — copy_from_user happens inside vfs_write / console_write. */
    return proc->fds[fd].ops->write(&proc->fds[fd],
                                    (const void *)(uintptr_t)buf, len);
}
```

No `printk` call in `sys_write`. No `fd == 1` special case. The path is:
`sys_write` → cap_check → fd table lookup → `console_write` → `printk`.

`CAP_KIND_VFS_WRITE` and `ENOCAP` are available via `proc.h` → `cap.h` (same include chain as `sys_open`'s cap check).

---

## Files Changed

| File | Change |
|------|--------|
| `kernel/mm/vmm.h` | Declare `vmm_free_user_pml4` |
| `kernel/mm/vmm.c` | Implement `vmm_free_user_pml4`; define `PTE_PS` locally; use existing `VMM_FLAG_PRESENT` |
| `kernel/sched/sched.h` | Add `stack_pages` field to `aegis_task_t` |
| `kernel/sched/sched.c` | `sched_spawn`: set `stack_pages = STACK_PAGES`; `sched_exit`: use `stack_pages`, add is_user scan for shutdown, call `vmm_free_user_pml4` |
| `kernel/core/main.c` | Rename `task_heartbeat` → `task_idle`; add `console_init()` after `vfs_init()` |
| `kernel/fs/console.h` | New — console device interface |
| `kernel/fs/console.c` | New — console VFS driver |
| `kernel/cap/cap.h` | Add `CAP_KIND_VFS_WRITE 2u` |
| `kernel/proc/proc.c` | Second `cap_grant`; pre-open fd 1; update printk to "2 capabilities" |
| `kernel/syscall/syscall.c` | Rework `sys_write`: cap check + fd table routing |
| `tests/expected/boot.txt` | `1 capability` → `2 capabilities` |

No Makefile changes — `kernel/fs/` already builds from Phase 10.

---

## Test Oracle

**HARD STOP:** The current `tests/expected/boot.txt` (Phase 9 baseline) differs substantially from this oracle — it contains `[USER]` output lines and `[CAP] OK: capability subsystem reserved`, none of which match the Phase 10+11+12 combined state shown below. Do not apply this oracle to `boot.txt` until Phase 10 and Phase 11 are both fully implemented and `make test` is GREEN against the Phase 11 oracle. Phase 12's only change to `boot.txt` is the single line noted below.

One line changes from Phase 11:

```
[CAP] OK: 2 capabilities granted to init
```

Full oracle (post Phase 10 + 11 + 12):

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
[CAP] OK: 2 capabilities granted to init
[VMM] OK: identity map removed
[SCHED] OK: scheduler started, 3 tasks
[MOTD] Hello from initrd!
[AEGIS] System halted.
```

**Prerequisite:** This oracle is only valid after Phase 10, 11, and 12 are all implemented. The `[VFS]`, `[INITRD]`, and `[MOTD]` lines come from Phase 10. `[CAP] OK: capability subsystem initialized` comes from Phase 11. The `2 capabilities` line is Phase 12's only oracle change.

---

## Success Criteria

1. `make test` exits 0.
2. No user address space pages remain allocated after process exit — verify with PMM free-page count before and after.
3. `grep -n 'is_user.*STACK_PAGES\|STACK_PAGES.*is_user' kernel/sched/sched.c` returns nothing.
4. `grep -n 'printk' kernel/syscall/syscall.c | grep sys_write` returns nothing (`sys_write` no longer calls `printk` directly; other syscall dispatch logging is unaffected).
5. Rust crate compiles without warnings under `cargo +nightly build --release`.

---

## Phase 13 Forward-Looking Constraints

**fd 0 and fd 2 are empty.** `proc->fds[0]` (stdin) and `proc->fds[2]` (stderr) are not pre-opened. When a real terminal driver exists (backed by the PS/2 keyboard ring buffer), Phase 13 should pre-open fd 0 to it and fd 2 to the console. A `read(0, ...)` call currently returns `EBADF` — correct behavior for Phase 12.

**`console_write` truncates at 256 bytes.** Sufficient for Phase 12. When the shell arrives, replace with a loop or increase the buffer.

**`CAP_KIND_VFS_WRITE` covers all writable fds.** Phase 12 has one writable fd (stdout). When multiple writable files exist (Phase 13+), consider whether write access to different resources should require distinct capability kinds or whether `VFS_WRITE` remains the coarse gate. The per-file permission model (if any) lives in the VFS layer, not the capability layer.

**Capability delegation still deferred.** A second user process spawned by Phase 13 must receive its capabilities via `proc_spawn` grants (same as init). `sys_cap_grant` for parent→child delegation remains Phase 13+ work.

**`vmm_free_user_pml4` assumes 4KB-only mappings.** `vmm_map_user_page` creates only 4KB PTEs, so the `PTE_PS` guards at PDPT and PD level should never fire. If Phase 13 introduces huge-page user mappings, revisit this function.
