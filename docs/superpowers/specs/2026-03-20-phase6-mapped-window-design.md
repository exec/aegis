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
VMM_WINDOW_VA = ARCH_KERNEL_VIRT_BASE + 0x200000   // 0xFFFFFFFF80200000
```

This address falls in `PD_kernel` entry 1 — currently NULL. A new BSS array
`s_window_pt[512]` serves as the PT page for this range. Its physical address
is computed at runtime from its link-time VMA:

```c
uint64_t win_phys = (uint64_t)(uintptr_t)s_window_pt
                    - ARCH_KERNEL_VIRT_BASE + ARCH_KERNEL_PHYS_BASE;
```

### Bootstrap Sequence (inside vmm_init)

`vmm_init()` has a legitimate bootstrap exception: it may still use
`phys_to_table()` and `zero_page()` internally while setting up the initial
5-table page structure, because the identity map is guaranteed active at that
point. This is the **last** permitted use of those helpers.

At the end of `vmm_init()`, before printing its OK lines:

```c
s_pd_kernel[1] = win_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
s_window_pte   = &s_window_pt[0];
```

After `vmm_init()` returns, all VMM operations use the window.

### Non-Reentrancy

One slot is sufficient. The kernel is single-core. VMM operations are called
only from contexts where interrupts are masked (syscall dispatch runs with
`IF=0` via `SFMASK`; page-table walks complete in a handful of instructions
before any re-entry is possible).

---

## Implementation

### New Static Data in vmm.c

```c
#define VMM_WINDOW_VA (ARCH_KERNEL_VIRT_BASE + 0x200000UL)

static uint64_t           s_window_pt[512];   /* BSS — PT for window range      */
static volatile uint64_t *s_window_pte;       /* → s_window_pt[0], set at init  */
```

### New Private Functions (replace phys_to_table + zero_page)

```c
static void *
vmm_window_map(uint64_t phys)
{
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

```c
static uint64_t
alloc_table(void)
{
    uint64_t phys = pmm_alloc_page();
    uint64_t *t   = vmm_window_map(phys);
    for (int i = 0; i < 512; i++) t[i] = 0;
    vmm_window_unmap();
    return phys;
}
```

### Walk-Function Pattern

Every call of the form `phys_to_table(entry & ~0xFFF)` becomes:

```c
uint64_t *tbl = vmm_window_map(entry & ~0xFFFULL);
/* read or write one entry */
vmm_window_unmap();
```

Because each step of the walk (PML4 → PDPT → PD → PT) maps one table, reads
or writes one entry, then unmaps, a single slot is sufficient for the
sequential walk.

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
1. Confirm all `phys_to_table` / identity-cast patterns are gone from all
   kernel subsystems (not just VMM).
2. Remove the `[0..4MB) → [0..4MB)` entries from `s_pd_identity` and flush
   the TLB.
3. Fix any remaining code that casts physical addresses to pointers (TCB
   allocations in `sched.c`, kernel-stack allocations in `sched.c` and
   `proc.c`).

**Single window slot.** Phase 7+ that introduces concurrent kernel threads or
DMA mappings may need additional slots. The `s_window_pt[512]` array has 511
unused entries reserved for that purpose.
