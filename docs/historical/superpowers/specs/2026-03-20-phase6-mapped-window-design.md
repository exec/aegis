# Phase 6: Mapped-Window Allocator Design

## Goal

Replace all identity-map-dependent `phys_to_table()` / `zero_page()` casts in
`kernel/mm/vmm.c` with a window-mapped mechanism. The identity map remains
active (teardown deferred to Phase 7), but after this phase no VMM runtime
operation depends on it. A new `[VMM] OK: mapped-window allocator active` boot
line confirms initialization.

---

## Background and Motivation

Every VMM page-table operation currently uses two helpers that cast physical
addresses directly to pointers:

```c
static uint64_t *phys_to_table(uint64_t phys) {
    return (uint64_t *)(uintptr_t)phys;   /* valid only while identity map active */
}
static void zero_page(uint64_t phys) {
    uint64_t *t = phys_to_table(phys);    /* same dependency */
    for (int i = 0; i < 512; i++) t[i] = 0;
}
```

These work because the identity window `[0x0, 0x400000)` maps every physical
address below 4 MB to the same virtual address. The PMM only allocates from
physical RAM, so page-table pages always fall in that window. Phase 5
forward-looking constraints require eliminating this dependency before the
identity map can ever be torn down.

---

## Architecture

### The Mapped-Window Slot

A *mapped-window slot* is a single fixed virtual address whose page-table entry
(PTE) is permanently allocated in kernel BSS. Accessing an arbitrary physical
page proceeds in three steps:

1. Write `phys | PRESENT | WRITABLE` into the slot's PTE.
2. Call `arch_vmm_invlpg(VMM_WINDOW_VA)` to flush the TLB for that VA.
3. Read or write through `VMM_WINDOW_VA`.
4. Clear the PTE and call `arch_vmm_invlpg` again (optional but defensive).

Because the PTE itself is a C pointer into the kernel BSS (mapped permanently
by `PT_kernel`), writing to it never requires a page-table walk.

### Virtual Address Assignment

```
VMM_WINDOW_VA = ARCH_KERNEL_VIRT_BASE + 0x600000   // 0xFFFFFFFF80600000
```

`vmm_init()` builds a `pd_hi` page-table page with:
- `pd_hi[0]` → 2 MB huge page (PA `0x000000–0x1FFFFF`, kernel image + BSS)
- `pd_hi[1]` → 2 MB huge page (PA `0x200000–0x3FFFFF`, kernel image continued)
- `pd_hi[2]` → allocated at runtime by `vmm_map_page(KSTACK_VA, ...)` (KSTACK_VA = `0xFFFFFFFF80400000`)
- `pd_hi[3]` → **currently NULL** — this is where the window PT is installed

`VMM_WINDOW_VA` is `0xFFFFFFFF80600000`, which is `pd_hi[3]`'s 2 MB range.
A new BSS array `s_window_pt[512]` serves as the PT page for this range.
Its physical address is computed at runtime from its link-time VMA:

```c
uint64_t win_phys = (uint64_t)(uintptr_t)s_window_pt - ARCH_KERNEL_VIRT_BASE;
```

This formula is valid because the linker script uses `AT(ADDR(.bss) - KERN_VMA)`,
so LMA = VMA - KERN_VMA. `ARCH_KERNEL_PHYS_BASE` is 0, so the physical address
is simply VMA minus the virtual base offset — no `+ ARCH_KERNEL_PHYS_BASE` term.

### Bootstrap Sequence (inside vmm_init)

`vmm_init()` has a legitimate bootstrap exception: it may still use
`phys_to_table()` and `zero_page()` internally while setting up the initial
page structure, because the identity map is guaranteed active at that point.
This is the **last** permitted use of those helpers.

`vmm_init()` holds a local pointer `pd_hi = phys_to_table(pd_hi_phys)` for the
duration of the function. Before calling `arch_vmm_load_pml4()`, this pointer
is still valid (identity map active). The window is installed using that local:

```c
/* Still inside vmm_init(), pd_hi local pointer in scope */
uint64_t win_phys = (uint64_t)(uintptr_t)s_window_pt - ARCH_KERNEL_VIRT_BASE;
pd_hi[3]     = win_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
s_window_pte = &s_window_pt[0];
```

`s_window_pt` is a static BSS array in the kernel image, mapped permanently by
`pd_hi[0]`/`pd_hi[1]` (huge pages). So `&s_window_pt[0]` is a valid higher-half
pointer both before and after the CR3 switch. After `vmm_init()` returns, all
VMM operations use the window.

### Non-Reentrancy

One slot is sufficient. The kernel is single-core. VMM operations are called
only from contexts where interrupts are masked (syscall dispatch runs with
`IF=0` via `SFMASK`; page-table walks complete in a handful of instructions
before any re-entry is possible).

---

## Implementation

### New Static Data in vmm.c

```c
#define VMM_WINDOW_VA (ARCH_KERNEL_VIRT_BASE + 0x600000UL)

static uint64_t           s_window_pt[512];   /* BSS — PT for window range      */
static volatile uint64_t *s_window_pte;       /* → s_window_pt[0], set at init  *
                                               * volatile: prevents the compiler  *
                                               * from caching the PTE value; each *
                                               * vmm_window_map write must reach  *
                                               * memory before the invlpg asm     *
                                               * barrier that follows it.         */
```

### New Private Functions (replace phys_to_table + zero_page)

```c
static void *
vmm_window_map(uint64_t phys)
{
    /* Write before invlpg: arch_vmm_invlpg is __asm__ volatile, which is a
     * compiler barrier. volatile on s_window_pte ensures the write is not
     * hoisted past other memory operations. Order is: write PTE → invlpg → use. */
    *s_window_pte = phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
    arch_vmm_invlpg(VMM_WINDOW_VA);
    return (void *)VMM_WINDOW_VA;
}

static void
vmm_window_unmap(void)
{
    *s_window_pte = 0;
    arch_vmm_invlpg(VMM_WINDOW_VA);
}
```

### alloc_table() Replacement

`alloc_table()` maps the new page, zeros it, then unmaps — one `invlpg` pair:

```c
static uint64_t
alloc_table(void)
{
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) {
        printk("[VMM] FAIL: out of memory allocating page table\n");
        for (;;) {}
    }
    uint64_t *t = vmm_window_map(phys);
    int i;
    for (i = 0; i < 512; i++) t[i] = 0;
    vmm_window_unmap();
    return phys;
}
```

### Walk-Function Pattern

A 4-level page-table walk reads one entry per level. Rather than map/unmap at
each level (8 `invlpg` calls), the window is remapped by overwriting the PTE
directly — `vmm_window_map` at each level, a single `vmm_window_unmap` at the
end. This halves the TLB flush count and avoids a window between unmap and
remap where a stale TLB entry could cause a silent bad read:

```c
/* Example: walk to PT level, then set leaf PTE */
uint64_t *pml4 = vmm_window_map(s_pml4_phys);
uint64_t  pdpt_phys = pml4[pml4_idx] & VMM_PAGE_MASK;

uint64_t *pdpt = vmm_window_map(pdpt_phys);   /* overwrites PTE, new invlpg */
uint64_t  pd_phys = pdpt[pdpt_idx] & VMM_PAGE_MASK;

uint64_t *pd = vmm_window_map(pd_phys);
uint64_t  pt_phys = pd[pd_idx] & VMM_PAGE_MASK;

uint64_t *pt = vmm_window_map(pt_phys);
pt[pt_idx] = phys | flags;

vmm_window_unmap();  /* single unmap at the end */
```

The single-slot non-reentrancy guarantee holds: each `vmm_window_map` call
overwrites the previous mapping before the next pointer is derived, so no two
levels are simultaneously live.

### Deletion

`phys_to_table()` and `zero_page()` are deleted from `vmm.c` once no call
sites remain.

---

## Files Changed

| File | Change |
|------|--------|
| `kernel/mm/vmm.c` | Add `s_window_pt`, `s_window_pte`, `VMM_WINDOW_VA`; add `vmm_window_map`/`vmm_window_unmap`; wire in `vmm_init`; replace all `phys_to_table`/`zero_page` call sites; delete both helpers |
| `kernel/mm/vmm.h` | No public API change — window is entirely internal |
| `tests/expected/boot.txt` | Add one line: `[VMM] OK: mapped-window allocator active` |
| `.claude/CLAUDE.md` | Update build status table; add Phase 6 forward-looking constraints |

No new files. No Makefile changes. No arch-layer changes (`arch_vmm_invlpg`
already declared in `arch.h`).

---

## Test Oracle

`tests/expected/boot.txt` gains one line immediately after the existing VMM
kernel-mapping line:

```
[VMM] OK: kernel mapped to 0xFFFFFFFF80000000
[VMM] OK: mapped-window allocator active
```

The test remains: `make test` exits 0 on exact match.

---

## Success Criteria

1. `make test` exits 0.
2. `phys_to_table` and `zero_page` no longer exist in `vmm.c`.
3. No `(uint64_t *)(uintptr_t)phys` casts remain in `vmm.c` outside of
   `vmm_window_map` itself.

---

## Phase 6 Forward-Looking Constraints

**Identity map still active.** The window mechanism is a prerequisite for
teardown but does not itself tear the identity map down. Phase 7 must:
1. Audit every identity-cast in the kernel before touching the page tables.
   Run the following from the repo root to get a complete list:
   ```bash
   grep -rn '(uint64_t \*)(uintptr_t)\|(uint8_t \*)(uintptr_t)\|(void \*)(uintptr_t)' kernel/
   ```
   Known sites at Phase 6 completion: TCB allocation in `sched.c`
   (`sched_spawn`), kernel-stack allocation in `sched.c` and `proc.c`. All
   must be migrated to a higher-half kernel allocator before the identity
   map is removed.
2. Remove the `[0..4MB) → [0..4MB)` entries from `pd_lo` and flush the TLB.
3. Verify with `make test` and a deliberate access of a low physical address
   to confirm a fault is triggered.

**Single window slot.** Phase 7+ that introduces concurrent kernel threads or
DMA mappings may need additional slots. The `s_window_pt[512]` array has 511
unused entries reserved for that purpose.
