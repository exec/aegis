# Phase 9: Syscall Infrastructure Cleanup + Process Memory Reclaim

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move `USER_ADDR_MAX`/`user_ptr_valid` to a shared header, replace the per-byte stac/clac loop in `sys_write` with a `copy_from_user` helper using a 256-byte kernel buffer, and add `kva_free_pages` wired into `sched_exit` so kernel objects (TCB, kernel stack, PCB) are returned to the PMM when a process exits.

**Architecture:** Three independent changes applied in order: header extraction (Tasks 1–3), kva free path (Task 4), then sched_exit deferred cleanup (Task 5). The boot.txt oracle is unchanged throughout — `make test` must exit 0 after every task. `vmm_unmap_page` is already present in `vmm.c`/`vmm.h` from Phase 8 work — do not re-implement it.

**Tech Stack:** C (freestanding, `-Wall -Wextra -Werror`), NASM (no changes this phase), x86-64 Aegis kernel. Build: `make`. Test: `make test` (QEMU headless, diff against `tests/expected/boot.txt`).

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `kernel/syscall/syscall_util.h` | **Create** | `USER_ADDR_MAX` constant + `user_ptr_valid` inline (validation only) |
| `kernel/mm/uaccess.h` | **Create** | `copy_from_user` static inline (transfer only) |
| `kernel/syscall/syscall.c` | **Modify** | Remove local defs; add new includes; rewrite `sys_write` with 256-byte chunking |
| `kernel/mm/kva.h` | **Modify** | Declare `kva_free_pages` |
| `kernel/mm/kva.c` | **Modify** | Implement `kva_free_pages` |
| `kernel/sched/sched.c` | **Modify** | Add deferred-free globals; wire cleanup at `sched_exit` entry and just before `ctx_switch` |
| `.claude/CLAUDE.md` | **Modify** | Mark Phase 9 done; replace Phase 9 forward-looking with Phase 10 constraints |

`kernel/mm/vmm.c` and `kernel/mm/vmm.h` are **not changed** — `vmm_unmap_page` is already present.

---

### Task 1: Create `kernel/syscall/syscall_util.h`

**Files:**
- Create: `kernel/syscall/syscall_util.h`

- [ ] **Step 1: Create the header**

Create `kernel/syscall/syscall_util.h` with this exact content:

```c
#ifndef SYSCALL_UTIL_H
#define SYSCALL_UTIL_H

#include <stdint.h>

/* USER_ADDR_MAX — highest canonical user-space virtual address (x86-64).
 * Shared by all syscall handlers. Do not redefine in individual syscall files. */
#define USER_ADDR_MAX 0x00007FFFFFFFFFFFUL

/* user_ptr_valid — return 1 if [addr, addr+len) lies entirely within the
 * canonical user address space, 0 otherwise.
 * For len=0, validates that addr itself is a canonical user address (does
 * NOT unconditionally pass — a kernel addr with len=0 still returns 0).
 * Overflow-safe: addr <= USER_ADDR_MAX - len avoids addr+len wraparound. */
static inline int
user_ptr_valid(uint64_t addr, uint64_t len)
{
    return len <= USER_ADDR_MAX && addr <= USER_ADDR_MAX - len;
}

#endif /* SYSCALL_UTIL_H */
```

- [ ] **Step 2: Verify the build is still clean**

```bash
make
```

Expected: clean build, no errors. The header is not yet included by anything — this is just a compile-path check.

- [ ] **Step 3: Commit**

```bash
git add kernel/syscall/syscall_util.h
git commit -m "feat: add syscall_util.h — shared USER_ADDR_MAX + user_ptr_valid"
```

---

### Task 2: Create `kernel/mm/uaccess.h`

**Files:**
- Create: `kernel/mm/uaccess.h`

- [ ] **Step 1: Create the header**

Create `kernel/mm/uaccess.h` with this exact content:

```c
#ifndef UACCESS_H
#define UACCESS_H

#include "arch.h"
#include <stdint.h>

/* copy_from_user — copy len bytes from user-space src to kernel-space dst.
 *
 * Caller MUST validate [src, src+len) with user_ptr_valid() before calling.
 * Uses a single arch_stac/arch_clac window around the entire copy — one pair
 * of AC-bit transitions regardless of len, replacing the per-byte pattern.
 *
 * The "memory" clobbers inside arch_stac/arch_clac prevent the compiler from
 * hoisting the memcpy before stac or sinking it past clac.
 *
 * No fault recovery: GCC may vectorize __builtin_memcpy, emitting multi-byte
 * loads. If [src, src+len) crosses a page boundary where the second page is
 * unmapped, a #PF fires with AC=1. There is no fixup table (Linux extable).
 * Caller must ensure the entire range is mapped before calling. */
static inline void
copy_from_user(void *dst, const void *src, uint64_t len)
{
    arch_stac();
    __builtin_memcpy(dst, src, len);
    arch_clac();
}

#endif /* UACCESS_H */
```

`#include "arch.h"` resolves to `kernel/arch/x86_64/arch.h` because `-Ikernel/arch/x86_64` is in global `CFLAGS` (Makefile line 21). `arch_stac()` and `arch_clac()` are static inlines declared there.

- [ ] **Step 2: Verify the build is still clean**

```bash
make
```

Expected: clean build, no errors.

- [ ] **Step 3: Commit**

```bash
git add kernel/mm/uaccess.h
git commit -m "feat: add uaccess.h — copy_from_user with single stac/clac window per call"
```

---

### Task 3: Migrate `syscall.c` — use new headers, rewrite `sys_write`

**Files:**
- Modify: `kernel/syscall/syscall.c`

This removes the local `#define USER_ADDR_MAX` and `user_ptr_valid` static inline, adds includes for the new shared headers, and rewrites `sys_write` to use a 256-byte kernel stack buffer with chunked `copy_from_user`.

- [ ] **Step 1: Verify baseline is GREEN**

```bash
make test
```

Expected: exits 0. Do not proceed if this fails.

- [ ] **Step 2: Replace `syscall.c`**

Rewrite `kernel/syscall/syscall.c` in full:

```c
#include "syscall.h"
#include "syscall_util.h"
#include "uaccess.h"
#include "sched.h"
#include "printk.h"
#include <stdint.h>

/*
 * sys_write — syscall 1
 *
 * arg1 = fd (ignored: all output goes to printk)
 * arg2 = user virtual address of buffer
 * arg3 = byte count
 *
 * Returns byte count on success, -14 (EFAULT) if [arg2, arg2+arg3) is not
 * a canonical user-space range.
 *
 * copy_from_user copies up to 256 bytes at a time into a kernel stack buffer
 * (one stac/clac pair per chunk), then feeds each character to printk
 * outside the AC window.
 */
static uint64_t
sys_write(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    (void)arg1;
    if (!user_ptr_valid(arg2, arg3))
        return (uint64_t)-14;   /* EFAULT */
    const char *s = (const char *)(uintptr_t)arg2;
    char kbuf[256];
    uint64_t offset = 0;
    while (offset < arg3) {
        uint64_t n = arg3 - offset;
        if (n > sizeof(kbuf)) n = sizeof(kbuf);
        copy_from_user(kbuf, s + offset, n);
        uint64_t j;
        for (j = 0; j < n; j++)
            printk("%c", kbuf[j]);
        offset += n;
    }
    return arg3;
}

/*
 * sys_exit — syscall 60
 *
 * arg1 = exit code (ignored for Phase 5)
 * Calls sched_exit() which never returns.
 */
static uint64_t
sys_exit(uint64_t arg1)
{
    (void)arg1;
    sched_exit();
    __builtin_unreachable();
}

uint64_t
syscall_dispatch(uint64_t num, uint64_t arg1,
                 uint64_t arg2, uint64_t arg3)
{
    switch (num) {
    case 1:  return sys_write(arg1, arg2, arg3);
    case 60: return sys_exit(arg1);
    default: return (uint64_t)-1;   /* ENOSYS */
    }
}
```

Changes from Phase 8:
- Removed `#include "arch.h"` (no longer needed — arch_stac/arch_clac are encapsulated in `copy_from_user`)
- Added `#include "syscall_util.h"` and `#include "uaccess.h"`
- Removed local `#define USER_ADDR_MAX` and `user_ptr_valid` static inline
- Rewrote `sys_write` body: 256-byte stack buffer + `copy_from_user` chunk loop

- [ ] **Step 3: Build**

```bash
make
```

Expected: clean build, no warnings.

- [ ] **Step 4: Run `make test`**

```bash
make test
```

Expected: exits 0. Boot output must match `tests/expected/boot.txt` exactly.

- [ ] **Step 5: Run success-criteria audits**

```bash
# USER_ADDR_MAX must be defined only in syscall_util.h
grep -rn 'USER_ADDR_MAX' kernel/
```

Expected: only `kernel/syscall/syscall_util.h` contains the `#define`. The macro is consumed inside `user_ptr_valid` in the same file and inlined into `syscall.c` at compile time. If you see a definition in `syscall.c`, the migration failed.

```bash
# arch_stac/arch_clac must not appear in syscall.c
grep -n 'arch_stac\|arch_clac' kernel/syscall/syscall.c
```

Expected: **no output**. These calls are now encapsulated inside `copy_from_user` in `uaccess.h`.

```bash
# Confirm copy_from_user call sites
grep -rn 'arch_stac()\|arch_clac()' kernel/
```

Expected: only `kernel/mm/uaccess.h` (the definitions). No lines in `syscall.c`.

- [ ] **Step 6: Commit**

```bash
git add kernel/syscall/syscall.c
git commit -m "refactor: syscall.c — use syscall_util.h + uaccess.h; chunked copy_from_user in sys_write"
```

---

### Task 4: Add `kva_free_pages`

**Files:**
- Modify: `kernel/mm/kva.h`
- Modify: `kernel/mm/kva.c`

`vmm_unmap_page` is already declared in `vmm.h` and implemented in `vmm.c`. Do not add a second definition.

`kva_free_pages` uses three existing functions (all headers already included by `kva.c`):
- `vmm_phys_of(uint64_t virt) → uint64_t` — returns physical address of a mapped VA; panics if not mapped
- `vmm_unmap_page(uint64_t virt)` — clears the PTE and calls `invlpg`; panics if not mapped
- `pmm_free_page(uint64_t phys)` — returns a physical frame to the PMM free list

- [ ] **Step 1: Add declaration to `kva.h`**

In `kernel/mm/kva.h`, add after the `kva_page_phys` declaration (before `#endif`):

```c
/* kva_free_pages — return n 4KB pages at va to the PMM.
 *
 * For each page i in [0, n): recover phys via vmm_phys_of, unmap the VA
 * via vmm_unmap_page, then return the frame via pmm_free_page. The virtual
 * address range is permanently abandoned — the bump cursor is not rewound;
 * VA space is not the scarce resource.
 *
 * va must be the base of a contiguous kva allocation. Panics if any page
 * is not mapped (calling with an unallocated VA is a kernel bug). */
void kva_free_pages(void *va, uint64_t n);
```

- [ ] **Step 2: Implement `kva_free_pages` in `kva.c`**

Add after `kva_page_phys` in `kernel/mm/kva.c`:

```c
void
kva_free_pages(void *va, uint64_t n)
{
    uint64_t addr = (uint64_t)(uintptr_t)va;
    uint64_t i;
    for (i = 0; i < n; i++) {
        uint64_t page_va = addr + i * 4096UL;
        uint64_t phys    = vmm_phys_of(page_va);
        vmm_unmap_page(page_va);
        pmm_free_page(phys);
    }
}
```

Note: `addr` is cast to `uint64_t` before the loop because pointer arithmetic on `void *` is undefined in standard C and a compile error under `-Werror`. The pattern matches `kva_page_phys` at line 50 of `kva.c`.

- [ ] **Step 3: Build**

```bash
make
```

Expected: clean build. `kva_free_pages` must link without "undefined reference."

- [ ] **Step 4: Run `make test`**

```bash
make test
```

Expected: exits 0. `kva_free_pages` is compiled and linked but not yet called — no behavior change.

- [ ] **Step 5: Commit**

```bash
git add kernel/mm/kva.h kernel/mm/kva.c
git commit -m "feat: kva_free_pages — return kva pages to PMM via vmm_unmap_page + pmm_free_page"
```

---

### Task 5: Wire deferred cleanup into `sched_exit`

**Files:**
- Modify: `kernel/sched/sched.c`

This is the most sensitive change in Phase 9. Read the rationale carefully before editing.

**Why deferred cleanup is required:**
- `ctx_switch` in `ctx_switch.asm` saves the dying task's `rsp` into `dying->rsp` before switching RSP. The TCB must remain valid memory until after the context switch.
- The dying task's kernel stack is the active CPU stack until `ctx_switch` switches RSP to `s_current->rsp`. Freeing the stack before the switch would fault the next instruction.

**The pattern:** record `dying`'s pointers in three static globals *before* `ctx_switch`, then free them at the *entry* of the *next* `sched_exit` call (by which point `ctx_switch` has long since completed and that stack is no longer live).

**Why `kva_free_pages(dying, 1)` frees the PCB:** for user processes, `dying == &proc->task`. Because `aegis_task_t task` is the first field of `aegis_process_t` (enforced by `_Static_assert(offsetof(aegis_process_t, task) == 0, ...)` in `proc.c`), `(void *)dying == (void *)proc`. Freeing 1 page at `dying` frees the entire PCB. For kernel tasks, `dying` points to the `aegis_task_t` directly and is itself the only allocation.

- [ ] **Step 1: Add the three deferred-cleanup globals**

In `kernel/sched/sched.c`, add the following block immediately before the `sched_exit` function definition. The right location is after the closing `}` of `sched_add` (around line 89) and before `void sched_exit(void)`:

```c
/* Deferred cleanup: dying task's resources cannot be freed before ctx_switch
 * (ctx_switch writes dying->rsp; the dying stack is live until RSP switches).
 * Record them here and free at the entry of the next sched_exit call. */
static void    *g_prev_dying_tcb         = NULL;
static void    *g_prev_dying_stack       = NULL;
static uint64_t g_prev_dying_stack_pages = 0;
```

- [ ] **Step 2: Add deferred free at `sched_exit` entry**

At the top of `sched_exit`, immediately after `vmm_switch_to(vmm_get_master_pml4())` (the current first line of the function body), add:

```c
    /* Free resources of the previously exited task (safe: ctx_switch has
     * completed; that TCB and stack are no longer live on any CPU). */
    if (g_prev_dying_tcb) {
        kva_free_pages(g_prev_dying_stack, g_prev_dying_stack_pages);
        kva_free_pages(g_prev_dying_tcb, 1);
        g_prev_dying_tcb = NULL;
    }
```

- [ ] **Step 3: Record globals and remove stale comment before `ctx_switch`**

The current end of `sched_exit` is (lines 128–132 of `sched.c`):

```c
    /* PHASE 8 CLEANUP: free dying->stack_base (kva pages) and dying itself
     * once a kernel page-table free path (vmm_unmap_page + pmm_free_page)
     * is available. See CLAUDE.md Phase 8 forward-looking constraints. */
    ctx_switch(dying, s_current);
    __builtin_unreachable();
```

Replace those five lines with:

```c
    /* Record dying task for deferred cleanup at the next sched_exit entry.
     * Must be set AFTER all list manipulation and BEFORE ctx_switch:
     * ctx_switch writes dying->rsp, so the TCB must remain valid until
     * after the RSP switch completes. */
    g_prev_dying_stack       = (void *)dying->stack_base;
    g_prev_dying_stack_pages = dying->is_user ? 1 : STACK_PAGES;
    g_prev_dying_tcb         = dying;
    ctx_switch(dying, s_current);
    __builtin_unreachable();
```

`is_user ? 1 : STACK_PAGES` encodes which allocation size was used:
- `proc_spawn` (user tasks): `kva_alloc_pages(1)` → 1 page for kernel stack
- `sched_spawn` (kernel tasks): `kva_alloc_pages(STACK_PAGES)` → `STACK_PAGES` (4) pages

`STACK_PAGES` is `#define`d at line 14 of `sched.c` — it's visible inside the file without any header.

- [ ] **Step 4: Build**

```bash
make
```

Expected: clean build, no warnings.

- [ ] **Step 5: Run `make test`**

```bash
make test
```

Expected: exits 0. The deferred cleanup runs silently after `sys_exit` — no new output lines.

- [ ] **Step 6: Commit**

```bash
git add kernel/sched/sched.c
git commit -m "feat: sched_exit — deferred TCB/stack cleanup via kva_free_pages"
```

---

### Task 6: Update CLAUDE.md

**Files:**
- Modify: `.claude/CLAUDE.md`

- [ ] **Step 1: Add Phase 9 cleanup row to the build status table**

Find the build status table. After the SMAP row, add:

```
| Syscall cleanup + kva free path (Phase 9) | ✅ Done | syscall_util.h + uaccess.h; copy_from_user in sys_write; kva_free_pages wired into sched_exit |
```

- [ ] **Step 2: Replace the Phase 9 forward-looking constraints section with Phase 10 constraints**

Find `### Phase 9 forward-looking constraints` and replace the entire section with:

```markdown
### Phase 10 forward-looking constraints

**`copy_to_user` not yet needed.** When `sys_read` arrives in Phase 10, add a symmetric
`copy_to_user(dst_user, src_kernel, len)` to `uaccess.h` alongside `copy_from_user`.
Pattern is identical: `arch_stac`, `memcpy`, `arch_clac`.

**`is_user ? 1 : STACK_PAGES` is a hidden contract.** Add a `stack_pages` field to
`aegis_task_t` and populate it at allocation time (`sched_spawn`, `proc_spawn`),
eliminating the inference. A kernel task with a non-`STACK_PAGES` stack size would
silently free the wrong number of pages.

**Last-exit leak.** Simplest fix: permanent idle task. Rename `task_heartbeat` to
`task_idle`, remove the 500-tick exit, and add a separate shutdown mechanism. A
non-exiting kernel task in the run queue ensures every exiting process gets cleaned
up by the deferred pattern at the next call to `sched_exit`.

**User address space teardown deferred.** `sched_exit` still leaks the user PML4
and ELF segment pages. Phase 10 must walk and free PML4 entries 0–255 only (the user
half). Entries 256–511 are the kernel half, shared with the master PML4 — touching
them would corrupt every other process.
```

- [ ] **Step 3: Update the last-updated timestamp**

Add or replace the last-updated line at the bottom of CLAUDE.md:

```
*Last updated: 2026-03-21 — Phase 9 complete, make test GREEN. syscall_util.h + uaccess.h introduced; copy_from_user in sys_write; kva_free_pages + deferred cleanup in sched_exit.*
```

- [ ] **Step 4: Final verification**

```bash
make test
```

Expected: exits 0. This is the Phase 9 green gate.

- [ ] **Step 5: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs: CLAUDE.md — mark Phase 9 complete; add Phase 10 constraints"
```
