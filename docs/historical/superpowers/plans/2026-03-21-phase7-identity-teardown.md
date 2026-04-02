# Phase 7: Identity Map Teardown Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate every physical-address-as-pointer cast from the kernel, introduce a kernel virtual allocator (`kva`) so TCBs/stacks/PCBs get higher-half VAs, give each user process its own kernel stack VA, then remove `pml4[0]` and verify the identity map is gone.

**Architecture:** A new `kernel/mm/kva.c` bump allocator maps PMM pages into the `pd_hi[4+]` virtual range (`0xFFFFFFFF80800000+`). Because `pd_hi` is shared between master and all user PML4s, kva-mapped pages are immediately visible in kernel context regardless of active CR3. Once all physical casts are replaced, `vmm_teardown_identity()` clears `pml4[0]` and reloads CR3.

**Tech Stack:** C, `kernel/mm/kva.c` (new), `kernel/mm/vmm.c` (two new functions), `kernel/sched/sched.c`, `kernel/proc/proc.c`, `kernel/elf/elf.c`, `kernel/core/main.c`.

---

## Background: What You Are Replacing

Read these files before starting. Key identity-map dependencies to eliminate:

| File | Line | Cast | Fix |
|------|------|------|-----|
| `kernel/sched/sched.c` | 44 | `(aegis_task_t *)(uintptr_t)tcb_phys` | `kva_alloc_pages(1)` |
| `kernel/sched/sched.c` | 65 | `(uint8_t *)(uintptr_t)p` | `kva_alloc_pages(STACK_PAGES)` |
| `kernel/proc/proc.c` | 69 | `(aegis_process_t *)(uintptr_t)tcb_phys` | `kva_alloc_pages(1)` |
| `kernel/elf/elf.c` | 91 | `(uint8_t *)(uintptr_t)first_phys` | `kva_alloc_pages(page_count)` |

**Not identity-map dependencies** (do not change):
- `syscall.c:28` — user virtual address, not a physical address
- `pmm.c:115` — VA→physical arithmetic, not a dereference
- `arch_syscall.c`, `gdt.c` — kernel symbol addresses cast to `uint64_t` for MSRs/GDTR

**Key VMM facts:**
- `vmm_window_map(phys)` / `vmm_window_unmap()` — already in vmm.c (Phase 6)
- `ensure_table_phys(parent_phys, idx, flags)` — already in vmm.c
- `PTE_ADDR(e)` macro — already in vmm.c (strips flags from entry, leaves 4KB-aligned phys)
- `s_pml4_phys` — static in vmm.c, holds physical address of master PML4
- `VMM_FLAG_PRESENT`, `VMM_FLAG_WRITABLE`, `VMM_FLAG_USER` — defined in vmm.h

**Key sched.c facts:**
- `STACK_PAGES = 4`, `STACK_SIZE = STACK_PAGES * 4096UL`
- `_Static_assert(offsetof(aegis_task_t, rsp) == 0, ...)` — rsp must be first field, enforced at compile time
- `sched_exit` switches to master PML4 first (keep this, just update its comment)

**Key proc.c facts:**
- `KSTACK_VA = 0xFFFFFFFF80400000ULL` — this fixed address is DELETED in Phase 7
- `STACK_SIZE = 4096UL` (single page for user process kernel stack)
- The kernel stack must be accessible when user PML4 is loaded — kva pages satisfy this because `pd_hi` is shared

---

## File Structure

| File | Role |
|------|------|
| `kernel/mm/kva.h` | Public API: `kva_init`, `kva_alloc_pages`, `kva_page_phys` |
| `kernel/mm/kva.c` | Bump allocator implementation |
| `kernel/mm/vmm.c` | Add `vmm_phys_of` + `vmm_teardown_identity` |
| `kernel/mm/vmm.h` | Declare the two new functions |
| `kernel/sched/sched.c` | Replace TCB + stack physical casts; update comments |
| `kernel/proc/proc.c` | Replace PCB + kstack physical casts; delete `KSTACK_VA` |
| `kernel/elf/elf.c` | Replace segment-write physical cast; use `kva_page_phys` |
| `kernel/core/main.c` | Wire `kva_init` + `vmm_teardown_identity` |
| `Makefile` | Add `kva.c` to `MM_SRCS` |
| `tests/expected/boot.txt` | Add two new boot lines |
| `.claude/CLAUDE.md` | Update build status table |

---

## Task 1: RED — Update boot.txt

**Files:**
- Modify: `tests/expected/boot.txt`

- [ ] **Step 1: Insert the two new boot lines**

Open `tests/expected/boot.txt`. The current file has 19 lines. Make two insertions:

1. After `[VMM] OK: mapped-window allocator active` (line 5), insert:
   ```
   [KVA] OK: kernel virtual allocator active
   ```

2. After `[SYSCALL] OK: SYSCALL/SYSRET enabled` (line 14 in the new file), insert:
   ```
   [VMM] OK: identity map removed
   ```

The final file must be exactly:
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
[VMM] OK: identity map removed
[SCHED] OK: scheduler started, 3 tasks
[USER] hello from ring 3
[USER] hello from ring 3
[USER] hello from ring 3
[USER] done
[AEGIS] System halted.
```

- [ ] **Step 2: Verify make test fails**

```bash
make -C /path/to/worktree test 2>&1 | tail -10
echo "Exit: $?"
```

Expected: exit 1. The diff should show both new lines as expected-but-missing.

- [ ] **Step 3: Commit**

```bash
git add tests/expected/boot.txt
git commit -m "test: RED — add KVA and identity-map-removed boot lines"
```

---

## Task 2: kva Module + Makefile + Wire kva_init

**Files:**
- Create: `kernel/mm/kva.h`
- Create: `kernel/mm/kva.c`
- Modify: `Makefile` (add kva.c to MM_SRCS)
- Modify: `kernel/core/main.c` (add kva_init call + include)

Wire `kva_init` into main.c now (before the callers change in Tasks 4–6) so that `s_kva_next` is set before any `kva_alloc_pages` call.

- [ ] **Step 1: Create `kernel/mm/kva.h`**

```c
#ifndef AEGIS_KVA_H
#define AEGIS_KVA_H

#include <stdint.h>

/* kva_init — initialise the kernel virtual allocator.
 * Must be called after vmm_init() (requires the mapped-window allocator).
 * Prints [KVA] OK on success. */
void kva_init(void);

/* kva_alloc_pages — allocate n 4KB pages, map them to consecutive higher-half
 * virtual addresses, and return the base VA as a pointer.
 * Panics on PMM exhaustion. Never pass VMM_FLAG_USER — kva pages are mapped
 * into pd_hi (shared with user PML4s); without USER the MMU blocks ring-3
 * access. */
void *kva_alloc_pages(uint64_t n);

/* kva_page_phys — return the physical address of the page mapped at va.
 * va must be a VA previously returned by kva_alloc_pages (or offset within
 * such a range). Panics if any page-table level is absent. */
uint64_t kva_page_phys(void *va);

#endif /* AEGIS_KVA_H */
```

- [ ] **Step 2: Create `kernel/mm/kva.c`**

```c
#include "kva.h"
#include "vmm.h"
#include "pmm.h"
#include "printk.h"
#include "arch.h"
#include <stdint.h>

/* KVA_BASE: pd_hi[4] range — 0x800000 bytes above ARCH_KERNEL_VIRT_BASE.
 * pd_hi[0..1] = kernel image huge pages
 * pd_hi[2]    = (formerly KSTACK_VA — now also kva range)
 * pd_hi[3]    = VMM_WINDOW_VA (Phase 6)
 * pd_hi[4+]   = kva bump range (this allocator) */
#define KVA_BASE (ARCH_KERNEL_VIRT_BASE + 0x800000UL)

static uint64_t s_kva_next;

void
kva_init(void)
{
    s_kva_next = KVA_BASE;
    printk("[KVA] OK: kernel virtual allocator active\n");
}

void *
kva_alloc_pages(uint64_t n)
{
    uint64_t base = s_kva_next;
    uint64_t i;
    for (i = 0; i < n; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            printk("[KVA] FAIL: out of memory\n");
            for (;;) {}
        }
        /* IMPORTANT: never pass VMM_FLAG_USER here. kva pages are mapped into
         * pd_hi which is shared with user PML4s (same physical pd_hi page).
         * Absent VMM_FLAG_USER, the MMU blocks user-mode access to these PTEs.
         * Setting VMM_FLAG_USER would expose all kernel objects to ring-3. */
        vmm_map_page(s_kva_next, phys, VMM_FLAG_WRITABLE);
        s_kva_next += 4096UL;
    }
    return (void *)base;
}

uint64_t
kva_page_phys(void *va)
{
    return vmm_phys_of((uint64_t)(uintptr_t)va);
}
```

- [ ] **Step 3: Add kva.c to Makefile MM_SRCS**

Find:
```makefile
MM_SRCS = \
    kernel/mm/pmm.c \
    kernel/mm/vmm.c
```

Replace with:
```makefile
MM_SRCS = \
    kernel/mm/pmm.c \
    kernel/mm/vmm.c \
    kernel/mm/kva.c
```

- [ ] **Step 4: Wire kva_init into main.c**

Add `#include "kva.h"` to the include block at the top of `kernel/core/main.c`.

Add `kva_init()` call immediately after `vmm_init()`:

```c
    vmm_init();             /* page tables, higher-half map — [VMM] OK       */
    kva_init();             /* kernel VA bump allocator — [KVA] OK           */
    arch_set_master_pml4(vmm_get_master_pml4());
```

- [ ] **Step 5: Build and verify**

```bash
make -C /path/to/worktree 2>&1 | head -20
echo "Build exit: $?"
```

Expected: build succeeds. (`vmm_phys_of` is not yet declared — kva.c calls it but vmm.h doesn't declare it yet. The build will fail here. If it does, proceed to Task 3 before committing this task.)

**Note:** Tasks 2 and 3 must be completed together before the build passes because `kva.c` calls `vmm_phys_of` which is added in Task 3. You may commit them together after Task 3 step 4 if the build fails here.

- [ ] **Step 6: Commit (after Task 3 if build fails here)**

```bash
git add kernel/mm/kva.h kernel/mm/kva.c Makefile kernel/core/main.c
git commit -m "feat: add kva bump allocator + wire kva_init into main.c"
```

---

## Task 3: vmm_phys_of and vmm_teardown_identity

**Files:**
- Modify: `kernel/mm/vmm.h`
- Modify: `kernel/mm/vmm.c`

`vmm_phys_of` uses the walk-overwrite window pattern (same pattern as `vmm_unmap_page` — read the existing implementation for reference). `vmm_teardown_identity` clears `pml4[0]` via the window and reloads CR3.

- [ ] **Step 1: Add declarations to vmm.h**

At the bottom of `kernel/mm/vmm.h`, before `#endif`, add:

```c
/* vmm_phys_of — return the physical address of the 4KB page mapped at virt.
 * Uses the walk-overwrite window pattern. Panics if any level is not present. */
uint64_t vmm_phys_of(uint64_t virt);

/* vmm_teardown_identity — clear pml4[0] (the [0..512GB) identity range)
 * and reload CR3 for a full TLB flush. Must be called after all kernel
 * objects have been allocated via kva (so no identity-map cast remains).
 * Prints [VMM] OK: identity map removed. */
void vmm_teardown_identity(void);
```

- [ ] **Step 2: Add vmm_phys_of to vmm.c**

Add this function after `vmm_unmap_page`. It follows the same walk-overwrite pattern (each `vmm_window_map` overwrites the PTE from the previous level; single `vmm_window_unmap` at the end):

```c
uint64_t
vmm_phys_of(uint64_t virt)
{
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    /* Walk-overwrite pattern: overwrite window PTE at each level without
     * an intervening unmap. Single vmm_window_unmap at the end. */
    uint64_t *pml4  = vmm_window_map(s_pml4_phys);
    uint64_t  pml4e = pml4[pml4_idx];
    if (!(pml4e & VMM_FLAG_PRESENT)) {
        vmm_window_unmap();
        printk("[VMM] FAIL: vmm_phys_of not mapped (pml4)\n");
        for (;;) {}
    }

    uint64_t *pdpt  = vmm_window_map(PTE_ADDR(pml4e));
    uint64_t  pdpte = pdpt[pdpt_idx];
    if (!(pdpte & VMM_FLAG_PRESENT)) {
        vmm_window_unmap();
        printk("[VMM] FAIL: vmm_phys_of not mapped (pdpt)\n");
        for (;;) {}
    }

    uint64_t *pd  = vmm_window_map(PTE_ADDR(pdpte));
    uint64_t  pde = pd[pd_idx];
    if (!(pde & VMM_FLAG_PRESENT)) {
        vmm_window_unmap();
        printk("[VMM] FAIL: vmm_phys_of not mapped (pd)\n");
        for (;;) {}
    }

    uint64_t *pt  = vmm_window_map(PTE_ADDR(pde));
    uint64_t  pte = pt[pt_idx];
    vmm_window_unmap();

    if (!(pte & VMM_FLAG_PRESENT)) {
        printk("[VMM] FAIL: vmm_phys_of not mapped (pt)\n");
        for (;;) {}
    }
    return PTE_ADDR(pte);
}
```

- [ ] **Step 3: Add vmm_teardown_identity to vmm.c**

Add after `vmm_phys_of`:

```c
void
vmm_teardown_identity(void)
{
    /* Clear pml4[0]: removes the entire [0..512GB) low identity range.
     * pdpt_lo and pd_lo pages remain allocated but are now unreachable;
     * they will be reclaimed when a kernel page-table free path exists. */
    uint64_t *pml4 = vmm_window_map(s_pml4_phys);
    pml4[0] = 0;
    vmm_window_unmap();
    /* Full CR3 reload for a complete TLB flush. invlpg of each individual
     * huge page would work but CR3 reload is simpler and more complete. */
    arch_vmm_load_pml4(s_pml4_phys);
    printk("[VMM] OK: identity map removed\n");
}
```

- [ ] **Step 4: Build**

```bash
make -C /path/to/worktree 2>&1 | head -20
echo "Build exit: $?"
```

Expected: clean build. `vmm_phys_of` is now declared and defined, so `kva.c` compiles. `vmm_teardown_identity` is declared but not yet called from `main.c`.

- [ ] **Step 5: Run make test**

```bash
make -C /path/to/worktree test 2>&1 | tail -10
echo "Exit: $?"
```

Expected: exit 1 (still RED — `vmm_teardown_identity` not wired yet, sched/proc/elf still use physical casts).

- [ ] **Step 6: Commit**

If Task 2 step 6 was deferred:
```bash
git add kernel/mm/kva.h kernel/mm/kva.c Makefile kernel/core/main.c \
        kernel/mm/vmm.h kernel/mm/vmm.c
git commit -m "feat: add kva allocator, vmm_phys_of, vmm_teardown_identity"
```

Otherwise (Task 2 already committed):
```bash
git add kernel/mm/vmm.h kernel/mm/vmm.c
git commit -m "feat: add vmm_phys_of and vmm_teardown_identity to vmm.c"
```

---

## Task 4: Replace Physical Casts in sched.c

**Files:**
- Modify: `kernel/sched/sched.c`

Replace the two identity-cast allocation blocks in `sched_spawn`. Update two stale comments in `sched_exit`. The `#include "kva.h"` addition is needed — `kernel/mm` is already in `CFLAGS -I` so no Makefile change needed.

- [ ] **Step 1: Add #include "kva.h" to sched.c**

At the top of `kernel/sched/sched.c`, add after the existing includes:
```c
#include "kva.h"
```

- [ ] **Step 2: Replace TCB allocation in sched_spawn**

Find this block (approximately lines 31–44):
```c
    /* Allocate TCB (one page from PMM — plenty of space).
     *
     * IDENTITY MAP DEPENDENCY: pmm_alloc_page() returns a physical address.
     * The cast to aegis_task_t * is valid only while the identity window
     * [0..4MB) is active. Phase 4 must not tear down the identity map before
     * replacing these raw physical casts with a mapped-window allocator.
     * See CLAUDE.md "Phase 3 forward-looking constraints". */
    uint64_t tcb_phys = pmm_alloc_page();
    if (!tcb_phys) {
        printk("[SCHED] FAIL: OOM allocating TCB\n");
        for (;;) {}
    }
    aegis_task_t *task = (aegis_task_t *)(uintptr_t)tcb_phys;
```

Replace with:
```c
    /* Allocate TCB (one kva page — higher-half VA, no identity-map dependency). */
    aegis_task_t *task = kva_alloc_pages(1);
```

- [ ] **Step 3: Replace stack allocation in sched_spawn**

Find this block (approximately lines 46–66):
```c
    /* Allocate stack (STACK_PAGES individual pages).
     *
     * CONTIGUITY ASSUMPTION: The Phase 3 PMM is a bitmap allocator over the
     * physical memory map. Early boot memory is a single contiguous range and
     * the bitmap allocates sequentially, so successive pmm_alloc_page() calls
     * return physically adjacent frames. This allows treating the pages as a
     * single STACK_SIZE region. If the PMM ever becomes non-sequential (e.g.
     * after buddy allocator introduction in Phase 5), this must be replaced
     * with a multi-page contiguous allocation.
     */
    uint8_t *stack = (void *)0;
    uint32_t i;
    for (i = 0; i < STACK_PAGES; i++) {
        uint64_t p = pmm_alloc_page();
        if (!p) {
            printk("[SCHED] FAIL: OOM allocating stack\n");
            for (;;) {}
        }
        if (i == 0)
            stack = (uint8_t *)(uintptr_t)p;
    }
```

Replace with:
```c
    /* Allocate stack (STACK_PAGES kva pages — consecutive VAs, no contiguity
     * assumption on physical addresses). */
    uint8_t *stack = kva_alloc_pages(STACK_PAGES);
```

- [ ] **Step 4: Update sched_exit top-of-function comment**

Find the comment block at the top of `sched_exit` that says something like "IDENTITY MAP DEPENDENCY" or "TCBs are in identity-mapped low memory." Replace it with:

```c
    /* Switch to master PML4 at entry (defensive measure).
     *
     * Before Phase 7: required because TCBs were in identity-mapped [0..4MB),
     * which is absent from user PML4s.
     * After Phase 7: TCBs are kva-mapped higher-half VAs, visible from any CR3
     * (pd_hi is shared). The switch is retained as a defensive measure. */
    vmm_switch_to(vmm_get_master_pml4());
```

- [ ] **Step 5: Update sched_exit bottom comment**

Find the comment near the bottom of `sched_exit` (around line 154) that says "switch back to master PML4 so its identity-mapped stack is accessible." Update to reference kva-mapped stacks:

```c
    /* If dying is a user task and the next task is a kernel task, switch to
     * master PML4. Kernel task stacks are kva-mapped (higher-half), visible
     * in any PML4, but the switch ensures a clean CR3 state for the resume. */
```

- [ ] **Step 6: Build**

```bash
make -C /path/to/worktree 2>&1 | head -20
echo "Build exit: $?"
```

Expected: clean build. The `pmm.h` include in `sched.c` is now unused — if `-Wunused` fires on it, remove the `#include "pmm.h"` line. (Check by looking at what other headers sched.c uses.)

- [ ] **Step 7: Commit**

```bash
git add kernel/sched/sched.c
git commit -m "refactor: sched_spawn uses kva_alloc_pages — remove identity-map casts"
```

---

## Task 5: Replace Physical Casts in proc.c — Delete KSTACK_VA

**Files:**
- Modify: `kernel/proc/proc.c`

Replace the PCB allocation and the fixed-KSTACK_VA kernel stack with kva allocations. Delete `#define KSTACK_VA`. Each call to `proc_spawn` now gets its own kernel stack VA from the bump allocator.

- [ ] **Step 1: Add #include "kva.h" to proc.c**

At the top of `kernel/proc/proc.c`, add after the existing includes:
```c
#include "kva.h"
```

- [ ] **Step 2: Delete KSTACK_VA and its comment block**

Find and delete the entire block:
```c
/*
 * KSTACK_VA — fixed higher-half virtual address for the user process kernel stack.
 * ...
 */
#define KSTACK_VA   0xFFFFFFFF80400000ULL
```

- [ ] **Step 3: Replace PCB allocation**

Find:
```c
    /* Allocate process control block (one PMM page) */
    uint64_t tcb_phys = pmm_alloc_page();
    if (!tcb_phys) {
        printk("[PROC] FAIL: OOM allocating process TCB\n");
        for (;;) {}
    }
    aegis_process_t *proc = (aegis_process_t *)(uintptr_t)tcb_phys;
```

Replace with:
```c
    /* Allocate process control block (one kva page — higher-half VA). */
    aegis_process_t *proc = kva_alloc_pages(1);
```

- [ ] **Step 4: Replace kernel stack allocation**

Find:
```c
    /* Allocate kernel stack (single 4KB page) and map it at a fixed higher-half
     * virtual address shared by both the master PML4 and all user PML4s.
     *
     * vmm_map_page() walks the master PML4 using the mapped-window allocator
     * (vmm_window_map/unmap) to access page-table pages. After vmm_map_page()
     * the kernel stack is accessible at KSTACK_VA in both the master PML4 and
     * the user PML4 (shared pdpt_hi PDPT page).  We then use KSTACK_VA (not
     * kstack_phys) for all subsequent pointer arithmetic so the stack is
     * reachable regardless of which PML4 is loaded in CR3. */
    uint64_t kstack_phys = pmm_alloc_page();
    if (!kstack_phys) {
        printk("[PROC] FAIL: OOM allocating kernel stack\n");
        for (;;) {}
    }
    vmm_map_page(KSTACK_VA, kstack_phys,
                 VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
    uint8_t *kstack = (uint8_t *)KSTACK_VA;
```

Replace with:
```c
    /* Allocate kernel stack (one kva page — per-process higher-half VA).
     * kva pages are mapped into pd_hi (shared with user PML4s), so this
     * stack VA is reachable regardless of which PML4 is loaded in CR3.
     * Each proc_spawn call gets a distinct VA; the single-KSTACK_VA
     * limitation from Phase 5 is now resolved. */
    uint8_t *kstack = kva_alloc_pages(1);
```

- [ ] **Step 5: Build**

```bash
make -C /path/to/worktree 2>&1 | head -20
echo "Build exit: $?"
```

Expected: clean build. If `-Wunused` fires on `#include "pmm.h"` in proc.c (pmm.h was needed for `pmm_alloc_page` which is now gone), remove that include line.

- [ ] **Step 6: Commit**

```bash
git add kernel/proc/proc.c
git commit -m "refactor: proc_spawn uses kva_alloc_pages — delete KSTACK_VA, per-process kernel stacks"
```

---

## Task 6: Replace Physical Cast in elf.c

**Files:**
- Modify: `kernel/elf/elf.c`

Replace the `(uint8_t *)(uintptr_t)first_phys` cast with `kva_alloc_pages(page_count)`. Use `kva_page_phys` to recover physical addresses for `vmm_map_user_page`.

- [ ] **Step 1: Add #include "kva.h" to elf.c**

At the top of `kernel/elf/elf.c`, add after the existing includes:
```c
#include "kva.h"
```

- [ ] **Step 2: Replace the segment allocation and write loop**

Find this block (approximately lines 70–105):
```c
        /* Allocate physically contiguous pages for this segment.
         *
         * CONTIGUITY ASSUMPTION: The Phase 3 bitmap PMM allocates sequentially,
         * so successive pmm_alloc_page() calls return physically adjacent frames.
         * This allows treating the pages as a single region for memcpy.
         * If the PMM becomes non-sequential (Phase 6+ buddy allocator),
         * replace this with a page-by-page copy. Same assumption as sched_spawn. */
        uint64_t page_count = (ph->p_memsz + 4095UL) / 4096UL;
        uint64_t first_phys = 0;
        uint64_t j;
        for (j = 0; j < page_count; j++) {
            uint64_t p = pmm_alloc_page();
            if (!p) {
                printk("[ELF] FAIL: OOM loading segment\n");
                for (;;) {}
            }
            if (j == 0)
                first_phys = p;
        }

        /* Copy file bytes into physical memory */
        uint8_t *dst = (uint8_t *)(uintptr_t)first_phys;
        const uint8_t *src = data + ph->p_offset;
        uint64_t k;
        for (k = 0; k < ph->p_filesz; k++)
            dst[k] = src[k];

        /* Zero BSS (bytes past p_filesz up to p_memsz) */
        for (k = ph->p_filesz; k < ph->p_memsz; k++)
            dst[k] = 0;

        /* Map each page into the user address space */
        uint64_t map_flags = VMM_FLAG_USER;
        if (ph->p_flags & PF_W)
            map_flags |= VMM_FLAG_WRITABLE;

        for (j = 0; j < page_count; j++) {
            vmm_map_user_page(pml4_phys,
                              ph->p_vaddr + j * 4096UL,
                              first_phys  + j * 4096UL,
                              map_flags);
        }
```

Replace with:
```c
        /* Allocate kva pages for this segment — no contiguity assumption on
         * physical frames; kva maps each PMM page to a consecutive kernel VA. */
        uint64_t page_count = (ph->p_memsz + 4095UL) / 4096UL;
        uint64_t j;

        uint8_t *dst = kva_alloc_pages(page_count);

        /* Copy file bytes through kernel VA */
        const uint8_t *src = data + ph->p_offset;
        uint64_t k;
        for (k = 0; k < ph->p_filesz; k++)
            dst[k] = src[k];

        /* Zero BSS (bytes past p_filesz up to p_memsz) */
        for (k = ph->p_filesz; k < ph->p_memsz; k++)
            dst[k] = 0;

        /* Map each page into the user address space.
         * kva_page_phys recovers the physical address of each page
         * (individual walk — O(page_count × 4) invlpg, acceptable at this scale). */
        uint64_t map_flags = VMM_FLAG_USER;
        if (ph->p_flags & PF_W)
            map_flags |= VMM_FLAG_WRITABLE;

        for (j = 0; j < page_count; j++) {
            vmm_map_user_page(pml4_phys,
                              ph->p_vaddr + j * 4096UL,
                              kva_page_phys(dst + j * 4096UL),
                              map_flags);
        }
```

- [ ] **Step 3: Remove unused pmm.h include (if needed)**

Check if `pmm_alloc_page` is still called anywhere in `elf.c`. If not, remove `#include "pmm.h"` from the top.

- [ ] **Step 4: Build**

```bash
make -C /path/to/worktree 2>&1 | head -20
echo "Build exit: $?"
```

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add kernel/elf/elf.c
git commit -m "refactor: elf_load uses kva_alloc_pages — remove identity-map cast"
```

---

## Task 7: Wire vmm_teardown_identity — GREEN

**Files:**
- Modify: `kernel/core/main.c`

Add the `vmm_teardown_identity()` call and run `make test` to confirm GREEN.

- [ ] **Step 1: Add vmm_teardown_identity call to main.c**

In `kernel_main`, find:
```c
    proc_spawn_init();      /* spawn init user process in ring 3             */
    sched_start();          /* prints [SCHED] OK, switches into first task   */
```

Replace with:
```c
    proc_spawn_init();      /* spawn init user process in ring 3             */
    /* All TCBs and stacks are in kva range at this point —
     * safe to remove the identity map. */
    vmm_teardown_identity(); /* pml4[0] = 0, CR3 reload — [VMM] OK          */
    sched_start();          /* prints [SCHED] OK, switches into first task   */
```

- [ ] **Step 2: Run make test**

```bash
make -C /path/to/worktree test 2>&1 | tail -15
echo "Exit: $?"
```

Expected: exit 0. Both new lines appear in the correct positions.

If exit 1: check the diff. Common issues:
- `[KVA] OK` or `[VMM] OK: identity map removed` missing → kva_init or vmm_teardown_identity not called
- `[SCHED]` line missing → crash before sched_start, likely a bad VA dereference. Check that all physical casts in sched/proc/elf were replaced.
- User process doesn't print → kstack VA not reachable from user PML4. Verify kva pages are in pd_hi range.

- [ ] **Step 3: Run grep audit**

```bash
grep -rn '(uintptr_t)' /path/to/worktree/kernel/ --include='*.c'
```

Every remaining hit must be one of:
- `arch_syscall.c` — VA→uint64_t for LSTAR MSR write
- `gdt.c` — VA→uint64_t for GDTR base
- `vmm.c` alloc_table_early / vmm_init bootstrap (have `// SAFETY:` comments)
- `vmm.c` vmm_window_map / vmm_phys_of / vmm_teardown_identity (VA→void* or phys→VA, not phys→ptr dereference)
- `pmm.c` — VA arithmetic, no dereference
- `syscall.c` — user VA from syscall arg, not a physical address
- `kva.c` — `(uint64_t)(uintptr_t)va` converting a VA to integer for vmm_phys_of

If you see any hit in `sched.c`, `proc.c`, or `elf.c` that looks like `(type *)(uintptr_t)some_phys`, that is a missed cast — fix it before proceeding.

- [ ] **Step 4: Commit**

```bash
git add kernel/core/main.c
git commit -m "feat: Phase 7 complete — wire vmm_teardown_identity, identity map removed"
```

---

## Task 8: Update CLAUDE.md

**Files:**
- Modify: `.claude/CLAUDE.md`

- [ ] **Step 1: Update build status table**

Find the "Mapped-window allocator" row and add a new row after it:

```
| Identity map teardown | ✅ Done | kva bump allocator at pd_hi[4+]; phys casts in sched/proc/elf eliminated; pml4[0] cleared |
```

- [ ] **Step 2: Add Phase 7 forward-looking constraints section**

After the Phase 6 forward-looking constraints section, add:

```markdown
### Phase 7 forward-looking constraints

**No remaining physical-address-as-pointer casts.** After Phase 7,
`grep -rn '(uintptr_t)' kernel/ --include='*.c'` must produce only
legitimate VA→integer or VA→arithmetic hits. Any new physical cast introduced
in Phase 8+ must carry a `// SAFETY:` comment and a forward-looking note for
removal.

**kva has no free path.** `kva_alloc_pages` is a one-way bump allocator.
The `sched_exit` code does not free TCB or stack pages (pre-existing leak).
Phase 8+ must introduce a slab or free-list and wire `pmm_free_page` +
`vmm_unmap_page` into the exit path.

**SMAP not enabled.** `sys_write` in `syscall.c` dereferences user virtual
addresses (`arg2`) directly with no bounds check. Phase 8 must add:
1. Bounds check: `arg2 + arg3 <= 0x00007FFFFFFFFFFF`
2. Enable SMAP (`CR4.SMAP`) so unintentional kernel→user dereferences fault
instead of silently succeeding.

**pdpt_lo and pd_lo pages are leaked.** Two pages allocated by `vmm_init`
for the now-removed identity map are unreachable. They are correctly marked
allocated in the PMM bitmap (won't be reused), but are wasted. Reclaim when
a kernel page-table free path exists.
```

- [ ] **Step 3: Update the "Last updated" timestamp**

Add a new `*Last updated:` line:
```
*Last updated: 2026-03-21 — Phase 7 complete, make test GREEN. Identity map removed; kva allocator live; per-process kernel stacks.*
```

- [ ] **Step 4: Run make test one final time**

```bash
make -C /path/to/worktree test 2>&1
echo "Exit: $?"
```

Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs: Phase 7 complete — update CLAUDE.md build status and forward-looking constraints"
```
