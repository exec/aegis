# Phase 6: Mapped-Window Allocator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace every `phys_to_table`/`zero_page` identity-map cast in `kernel/mm/vmm.c` with a window-mapped mechanism, eliminating the VMM's runtime dependency on the `[0..4MB)` identity mapping.

**Architecture:** A single fixed virtual address (`VMM_WINDOW_VA = 0xFFFFFFFF80600000`) is reserved in the kernel higher-half. Its PTE lives in a BSS array (`s_window_pt[512]`) that is permanently mapped by the existing kernel huge-page entries. To access any physical page-table page, write its address into the PTE, call `invlpg`, use the pointer, then clear and `invlpg` again. The identity map stays active (teardown is Phase 7) but no VMM function except the bootstrap inside `vmm_init` touches it.

**Tech Stack:** C (`kernel/mm/vmm.c`), `arch_vmm_invlpg` (already in `arch.h`).

---

## File Structure

Only one source file changes substantively:

| File | Change |
|------|--------|
| `tests/expected/boot.txt` | +1 line: `[VMM] OK: mapped-window allocator active` |
| `kernel/mm/vmm.c` | Add window infra; restructure all walk functions; delete `phys_to_table`/`zero_page` |
| `kernel/mm/vmm.h` | No changes — window is entirely internal |
| `.claude/CLAUDE.md` | Update build status table |

---

## Background: Current vmm.c Call Graph

Read `kernel/mm/vmm.c` before starting. Key facts:

- `zero_page(phys)` — casts phys to pointer, zeros 512 entries. Called only from `alloc_table`.
- `phys_to_table(phys)` — casts phys to `uint64_t *`. Called from `vmm_init` (bootstrap — OK to keep there) and from every runtime walk function.
- `alloc_table()` — allocates a PMM page, calls `zero_page`, returns physical address.
- `ensure_table(parent, idx)` — takes an already-cast pointer; if entry missing, calls `alloc_table`, installs entry. Returns child physical address.
- `ensure_table_user(parent, idx)` — same as `ensure_table` but adds `VMM_FLAG_USER`.
- Walk functions: `vmm_map_page`, `vmm_unmap_page`, `vmm_create_user_pml4`, `vmm_map_user_page`.

**Critical reentrancy constraint:** `ensure_table` (and `_user`) currently take a `uint64_t *parent` pointer. After Phase 6, that pointer IS `(void*)VMM_WINDOW_VA`. If `ensure_table` needs to call `alloc_table` (because the entry doesn't exist), `alloc_table` will call `vmm_window_map` and overwrite the window — instantly invalidating the `parent` pointer. The fix is to change `ensure_table` to accept a **physical address** instead of a pointer, so it can unmap before allocating and re-map to install. See Task 3.

---

## Task 1: RED — Update boot.txt

**Files:**
- Modify: `tests/expected/boot.txt`

The kernel will soon emit a second `[VMM]` line. Update the oracle first so the test is red before implementation begins.

- [ ] **Step 1: Edit tests/expected/boot.txt**

After line 4 (`[VMM] OK: kernel mapped to 0xFFFFFFFF80000000`), insert:

```
[VMM] OK: mapped-window allocator active
```

The file should now have 19 lines. Lines 4–5:
```
[VMM] OK: kernel mapped to 0xFFFFFFFF80000000
[VMM] OK: mapped-window allocator active
```

- [ ] **Step 2: Verify make test fails**

```bash
make -C /path/to/worktree test 2>&1 | tail -20
echo "Exit: $?"
```

Expected: exit 1, diff showing the new line is expected but not yet emitted.

- [ ] **Step 3: Commit**

```bash
git add tests/expected/boot.txt
git commit -m "test: RED — add mapped-window allocator boot line"
```

---

## Task 2: Window Infrastructure — Add to vmm.c and Wire into vmm_init

**Files:**
- Modify: `kernel/mm/vmm.c` (top of file + inside `vmm_init`)

Add the window slot constants, BSS array, and two private functions. Wire the PT into `pd_hi[3]` inside `vmm_init` using the local `pd_hi` pointer (which is in scope and identity-mapped during bootstrap). Print the new OK line.

**Key facts about vmm_init (read the file):**
- `pd_hi` is a local `uint64_t *` pointer created by `phys_to_table(pd_hi_phys)` at line 89.
- `pd_hi[0]` = 2 MB huge page PA `0x000000`, `pd_hi[1]` = 2 MB huge page PA `0x200000`. These cover all of kernel code+BSS.
- `pd_hi[2]` and `pd_hi[3]` are currently `0` (NULL).
- `KSTACK_VA = 0xFFFFFFFF80400000` maps to `pd_hi[2]` — do NOT touch it.
- `pd_hi[3]` covers `0xFFFFFFFF80600000–0xFFFFFFFF807FFFFF` — currently NULL. This is our window range.
- `s_window_pt` is a static BSS array. Its physical address = `(uint64_t)(uintptr_t)s_window_pt - ARCH_KERNEL_VIRT_BASE + ARCH_KERNEL_PHYS_BASE`. Valid because BSS is in the kernel image segment loaded at `ARCH_KERNEL_PHYS_BASE`.

- [ ] **Step 1: Add window constants and data after the existing `s_pml4_phys` declaration**

```c
/* Mapped-window allocator (Phase 6).
 * A single virtual address whose PTE is permanently allocated in BSS.
 * vmm_window_map(phys) installs phys into the PTE and flushes TLB.
 * vmm_window_unmap() clears the PTE and flushes TLB.
 * The window is non-reentrant: never hold it across any call that may
 * itself call vmm_window_map (e.g. alloc_table). */
#define VMM_WINDOW_VA (ARCH_KERNEL_VIRT_BASE + 0x600000UL)

static uint64_t           s_window_pt[512]; /* BSS — PT for window range       */
static volatile uint64_t *s_window_pte;     /* → s_window_pt[0], set at init   *
                                             * volatile: prevents the compiler  *
                                             * from caching the PTE value; each *
                                             * write must reach memory before   *
                                             * the __asm__ volatile invlpg.     */
```

- [ ] **Step 2: Add vmm_window_map and vmm_window_unmap after the `phys_to_table` function**

```c
/*
 * vmm_window_map — map an arbitrary physical page into the window slot.
 * Returns a pointer to VMM_WINDOW_VA, now backed by phys.
 *
 * Write ordering: the write to *s_window_pte must reach memory before the
 * invlpg asm barrier. volatile on s_window_pte ensures the compiler does not
 * hoist the write. arch_vmm_invlpg is __asm__ volatile, which also acts as
 * a compiler barrier — so the write-then-invlpg ordering is guaranteed.
 *
 * Do NOT call this while a previous vmm_window_map result is still in use
 * unless you are intentionally overwriting the mapping (walk-overwrite pattern).
 */
static void *
vmm_window_map(uint64_t phys)
{
    *s_window_pte = phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
    arch_vmm_invlpg(VMM_WINDOW_VA);
    return (void *)VMM_WINDOW_VA;
}

/*
 * vmm_window_unmap — clear the window PTE and flush TLB.
 * Call this after the last use of any vmm_window_map result.
 */
static void
vmm_window_unmap(void)
{
    *s_window_pte = 0;
    arch_vmm_invlpg(VMM_WINDOW_VA);
}
```

- [ ] **Step 3: Wire the window PT into vmm_init**

Inside `vmm_init`, BEFORE the `arch_vmm_load_pml4(pml4_phys)` call (so the identity map is still active and `pd_hi` local pointer is valid), add:

```c
    /* Install the mapped-window PT into pd_hi[3].
     * pd_hi is a local pointer (phys_to_table(pd_hi_phys)) in scope here.
     * pd_hi[0] and pd_hi[1] are the two 2MB kernel huge pages.
     * pd_hi[2] is used by KSTACK_VA at runtime — do not touch.
     * pd_hi[3] covers 0xFFFFFFFF80600000 (VMM_WINDOW_VA) — currently NULL. */
    {
        uint64_t win_phys = (uint64_t)(uintptr_t)s_window_pt
                            - ARCH_KERNEL_VIRT_BASE + ARCH_KERNEL_PHYS_BASE;
        pd_hi[3]     = win_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
        s_window_pte = &s_window_pt[0];
    }
```

After `arch_vmm_load_pml4(pml4_phys)`, add the second OK line:

```c
    printk("[VMM] OK: mapped-window allocator active\n");
```

The final tail of `vmm_init` should look like:

```c
    /* ... pd_hi[0..1] huge pages already set above ... */

    /* Install mapped-window PT into pd_hi[3] (VMM_WINDOW_VA) */
    {
        uint64_t win_phys = (uint64_t)(uintptr_t)s_window_pt
                            - ARCH_KERNEL_VIRT_BASE + ARCH_KERNEL_PHYS_BASE;
        pd_hi[3]     = win_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
        s_window_pte = &s_window_pt[0];
    }

    s_pml4_phys = pml4_phys;
    arch_vmm_load_pml4(pml4_phys);

    printk("[VMM] OK: kernel mapped to 0xFFFFFFFF80000000\n");
    printk("[VMM] OK: mapped-window allocator active\n");
}
```

- [ ] **Step 4: Build and run make test**

```bash
make -C /path/to/worktree test 2>&1 | tail -20
echo "Exit: $?"
```

Expected: exit 0. The new printk line is emitted; `phys_to_table` still works in runtime functions because the identity map is still active. Both conditions are satisfied.

If exit 1, check the diff — most likely a printk ordering issue.

- [ ] **Step 5: Commit**

```bash
git add kernel/mm/vmm.c
git commit -m "feat: add mapped-window allocator infrastructure to vmm_init"
```

---

## Task 3: Replace alloc_table and ensure_table

**Files:**
- Modify: `kernel/mm/vmm.c` (functions `alloc_table`, `ensure_table`, `ensure_table_user`)

Replace `alloc_table` to zero via the window. Replace `ensure_table` and `ensure_table_user` with a single `ensure_table_phys` that takes a physical address (not a pointer) so it can safely call `alloc_table` without a stale-pointer hazard.

**Why physical address, not pointer:**
`ensure_table_phys(parent_phys, idx, extra_flags)` maps the parent, reads the entry, **unmaps** before calling `alloc_table` (which uses the window internally), then re-maps parent to install the new child. This is the only safe pattern when allocation might be needed.

- [ ] **Step 1: Replace alloc_table**

```c
/*
 * alloc_table — allocate a page-table page from the PMM and zero it.
 * Uses vmm_window_map/unmap to zero the page without the identity map.
 * Panics if the PMM is exhausted.
 */
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
    for (i = 0; i < 512; i++)
        t[i] = 0;
    vmm_window_unmap();
    return phys;
}
```

- [ ] **Step 2: Replace ensure_table and ensure_table_user with ensure_table_phys**

Delete `ensure_table` and `ensure_table_user`. Add:

```c
/*
 * ensure_table_phys — if parent_table[idx] has no present child, allocate one.
 * Returns the physical address of the (possibly newly created) child table.
 *
 * Takes parent_phys (physical address) rather than a pointer, so it can
 * safely call alloc_table (which uses the window) without a stale-pointer
 * hazard: parent is unmapped before alloc_table is called, then re-mapped
 * to install the new child entry.
 *
 * extra_flags: 0 for kernel tables, VMM_FLAG_USER for user-accessible tables.
 * CRITICAL for user tables: ALL intermediate entries in a user walk must have
 * VMM_FLAG_USER set. The MMU checks USER at every level (PML4, PDPT, PD).
 */
static uint64_t
ensure_table_phys(uint64_t parent_phys, uint64_t idx, uint64_t extra_flags)
{
    uint64_t *parent = vmm_window_map(parent_phys);
    uint64_t  entry  = parent[idx];
    vmm_window_unmap();                   /* unmap before potential alloc_table */

    if (!(entry & VMM_FLAG_PRESENT)) {
        uint64_t child = alloc_table();   /* uses window internally */
        parent = vmm_window_map(parent_phys);
        parent[idx] = child | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | extra_flags;
        vmm_window_unmap();
        return child;
    }
    return PTE_ADDR(entry);
}
```

- [ ] **Step 3: Build and run make test**

```bash
make -C /path/to/worktree test 2>&1 | tail -20
echo "Exit: $?"
```

Expected: exit 0. `alloc_table` now uses the window; `ensure_table_phys` exists. Walk functions still use old `ensure_table`/`ensure_table_user` via `phys_to_table` — they'll be updated in Task 4. Since `ensure_table` and `ensure_table_user` are now deleted, the build will fail if any walk function still references them. Verify the build step first: if build fails, that's expected and confirms Task 4 is needed; if it passes, `make test` should exit 0.

Actually: the walk functions call `ensure_table(pml4, pml4_idx)` where `pml4` was obtained via `phys_to_table`. These must be updated in Task 4. If the build fails at this step, that is fine — it just means the walk functions need to be updated before committing. In that case, proceed to Task 4 before committing.

- [ ] **Step 4: Commit (only after build passes)**

```bash
git add kernel/mm/vmm.c
git commit -m "refactor: replace alloc_table zero_page + ensure_table with window-safe versions"
```

---

## Task 4: Replace Walk Functions (vmm_map_page, vmm_unmap_page, vmm_create_user_pml4, vmm_map_user_page)

**Files:**
- Modify: `kernel/mm/vmm.c` (four walk functions)

Replace all `phys_to_table` calls in the walk functions. Each function uses `ensure_table_phys` (for intermediate levels) and `vmm_window_map`/`vmm_window_unmap` (for the leaf operation).

**Walk-overwrite pattern** (safe when NO alloc_table call is possible): map → read entry → overwrite with next level → ... → single unmap at end. Used in `vmm_unmap_page`.

**Ensure-then-write pattern** (safe when alloc_table may be called): `ensure_table_phys` handles map/read/unmap/alloc/re-map internally. Used in `vmm_map_page` and `vmm_map_user_page`.

**Two-pass copy** (for `vmm_create_user_pml4`): read into stack buffer, then write from stack buffer. Required when two physical pages must be accessed and only one slot exists.

- [ ] **Step 1: Replace vmm_map_page**

```c
void
vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (virt & ~VMM_PAGE_MASK) {
        printk("[VMM] FAIL: vmm_map_page virt not aligned\n");
        for (;;) {}
    }
    if (phys & ~VMM_PAGE_MASK) {
        printk("[VMM] FAIL: vmm_map_page phys not aligned\n");
        for (;;) {}
    }

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t pdpt_phys = ensure_table_phys(s_pml4_phys, pml4_idx, 0);
    uint64_t pd_phys   = ensure_table_phys(pdpt_phys,   pdpt_idx, 0);
    uint64_t pt_phys   = ensure_table_phys(pd_phys,     pd_idx,   0);

    uint64_t *pt = vmm_window_map(pt_phys);
    if (pt[pt_idx] & VMM_FLAG_PRESENT) {
        printk("[VMM] FAIL: vmm_map_page double-map\n");
        for (;;) {}
    }
    pt[pt_idx] = phys | flags | VMM_FLAG_PRESENT;
    vmm_window_unmap();
}
```

- [ ] **Step 2: Replace vmm_unmap_page**

Uses the walk-overwrite pattern (no alloc_table, just reads + one write):

```c
void
vmm_unmap_page(uint64_t virt)
{
    if (virt & ~VMM_PAGE_MASK) {
        printk("[VMM] FAIL: vmm_unmap_page virt not aligned\n");
        for (;;) {}
    }

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    /* Walk-overwrite pattern: each vmm_window_map overwrites the previous
     * mapping. No alloc_table is called, so no reentrancy hazard. */
    uint64_t *pml4 = vmm_window_map(s_pml4_phys);
    if (!(pml4[pml4_idx] & VMM_FLAG_PRESENT)) {
        printk("[VMM] FAIL: vmm_unmap_page not mapped (pml4)\n");
        for (;;) {}
    }
    uint64_t pdpt_phys = PTE_ADDR(pml4[pml4_idx]);

    uint64_t *pdpt = vmm_window_map(pdpt_phys);
    if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT)) {
        printk("[VMM] FAIL: vmm_unmap_page not mapped (pdpt)\n");
        for (;;) {}
    }
    uint64_t pd_phys = PTE_ADDR(pdpt[pdpt_idx]);

    uint64_t *pd = vmm_window_map(pd_phys);
    if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) {
        printk("[VMM] FAIL: vmm_unmap_page not mapped (pd)\n");
        for (;;) {}
    }
    if (pd[pd_idx] & (1UL << 7)) {
        printk("[VMM] FAIL: vmm_unmap_page called on huge-page-backed address\n");
        for (;;) {}
    }
    uint64_t pt_phys = PTE_ADDR(pd[pd_idx]);

    uint64_t *pt = vmm_window_map(pt_phys);
    if (!(pt[pt_idx] & VMM_FLAG_PRESENT)) {
        printk("[VMM] FAIL: vmm_unmap_page not mapped (pt)\n");
        for (;;) {}
    }
    pt[pt_idx] = 0;
    vmm_window_unmap();
    arch_vmm_invlpg(virt);
}
```

- [ ] **Step 3: Replace vmm_create_user_pml4**

Two-pass copy via stack buffer (256 × 8 bytes = 2 KB; well within the 4 KB process kernel stack):

```c
uint64_t
vmm_create_user_pml4(void)
{
    uint64_t new_pml4_phys = alloc_table();   /* zeroed by alloc_table */
    uint64_t hi[256];                         /* 2 KB stack buffer for high entries */
    int i;

    /* Pass 1: read high entries [256..511] from master PML4 */
    uint64_t *master = vmm_window_map(s_pml4_phys);
    for (i = 0; i < 256; i++)
        hi[i] = master[256 + i];
    vmm_window_unmap();

    /* Pass 2: write them into the new PML4.
     * This makes the kernel higher-half accessible in every user process's
     * address space, so syscall handlers can execute after SYSCALL without
     * a CR3 switch. */
    uint64_t *newpml = vmm_window_map(new_pml4_phys);
    for (i = 0; i < 256; i++)
        newpml[256 + i] = hi[i];
    vmm_window_unmap();

    return new_pml4_phys;
}
```

- [ ] **Step 4: Replace vmm_map_user_page**

```c
void
vmm_map_user_page(uint64_t pml4_phys, uint64_t virt,
                  uint64_t phys, uint64_t flags)
{
    if (virt & ~VMM_PAGE_MASK) {
        printk("[VMM] FAIL: vmm_map_user_page virt not aligned\n");
        for (;;) {}
    }
    if (phys & ~VMM_PAGE_MASK) {
        printk("[VMM] FAIL: vmm_map_user_page phys not aligned\n");
        for (;;) {}
    }

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t pdpt_phys = ensure_table_phys(pml4_phys, pml4_idx, VMM_FLAG_USER);
    uint64_t pd_phys   = ensure_table_phys(pdpt_phys, pdpt_idx, VMM_FLAG_USER);
    uint64_t pt_phys   = ensure_table_phys(pd_phys,   pd_idx,   VMM_FLAG_USER);

    uint64_t *pt = vmm_window_map(pt_phys);
    if (pt[pt_idx] & VMM_FLAG_PRESENT) {
        printk("[VMM] FAIL: vmm_map_user_page double-map\n");
        for (;;) {}
    }
    pt[pt_idx] = phys | flags | VMM_FLAG_PRESENT;
    vmm_window_unmap();
}
```

- [ ] **Step 5: Build and run make test**

```bash
make -C /path/to/worktree test 2>&1 | tail -20
echo "Exit: $?"
```

Expected: exit 0. All walk functions now use `ensure_table_phys` and `vmm_window_map`; `phys_to_table` is still defined (will be deleted in Task 5) but no longer called from runtime functions.

- [ ] **Step 6: Commit**

```bash
git add kernel/mm/vmm.c
git commit -m "refactor: replace phys_to_table with vmm_window_map in all walk functions"
```

---

## Task 5: Delete phys_to_table and zero_page — GREEN

**Files:**
- Modify: `kernel/mm/vmm.c`

Remove the two identity-map-dependent helpers. Verify with grep that no call sites remain. Run `make test` to confirm GREEN.

- [ ] **Step 1: Inline the vmm_init bootstrap calls to phys_to_table**

`vmm_init` has 5 calls to `phys_to_table` (lines 85–89 in the original file)
that set up local pointers for the bootstrap table construction. These are the
last legitimate identity-map uses. Replace each one with a direct cast and a
`// SAFETY:` comment:

```c
    /* SAFETY: identity map [0..4MB) is active during vmm_init bootstrap;
     * alloc_table() only returns pages from PMM-managed low memory. */
    uint64_t *pml4    = (uint64_t *)(uintptr_t)pml4_phys;
    uint64_t *pdpt_lo = (uint64_t *)(uintptr_t)pdpt_lo_phys;
    uint64_t *pd_lo   = (uint64_t *)(uintptr_t)pd_lo_phys;
    uint64_t *pdpt_hi = (uint64_t *)(uintptr_t)pdpt_hi_phys;
    uint64_t *pd_hi   = (uint64_t *)(uintptr_t)pd_hi_phys;
```

These casts remain valid because: (a) the identity map is still active at this
point, (b) the pages were just allocated from the PMM which only returns
addresses below 4 MB, and (c) `arch_vmm_load_pml4` hasn't been called yet.
No other call to `phys_to_table` will remain after Tasks 3–4.

- [ ] **Step 2: Delete zero_page**

Remove the entire `zero_page` function (the comment block + function body, ~16 lines).

- [ ] **Step 3: Delete phys_to_table**

Remove the entire `phys_to_table` function (~7 lines).

- [ ] **Step 5: Verify no remaining call sites**

```bash
grep -n 'phys_to_table\|zero_page\|(uint64_t \*)(uintptr_t)\|(uint8_t \*)(uintptr_t)' \
    /path/to/worktree/kernel/mm/vmm.c
```

Expected: no matches. (The `vmm_window_map` function itself has `(void *)(uintptr_t)VMM_WINDOW_VA` — that VA cast is fine because it's the window VA, not a physical address. The `vmm_init` bootstrap casts inlined in Step 1 use `(uint64_t *)(uintptr_t)` but those are intentional identity-map bootstrap uses with `// SAFETY:` comments.)

- [ ] **Step 6: Build and run make test**

```bash
make -C /path/to/worktree 2>&1 | head -30
make -C /path/to/worktree test 2>&1 | tail -20
echo "Exit: $?"
```

Expected: clean build, exit 0.

- [ ] **Step 7: Commit**

```bash
git add kernel/mm/vmm.c
git commit -m "feat: Phase 6 complete — remove phys_to_table and zero_page from vmm.c"
```

---

## Task 6: Update CLAUDE.md

**Files:**
- Modify: `.claude/CLAUDE.md`

Update the build status table and add the Phase 6 forward-looking constraints.

- [ ] **Step 1: Update build status table**

Find the line:
```
| Virtual memory / paging | ✅ Done | Higher-half kernel at 0xFFFFFFFF80000000; 5-table setup (identity + kernel); identity map kept; teardown deferred to Phase 4 |
```

Add a new row after it:
```
| Mapped-window allocator | ✅ Done | VMM_WINDOW_VA=0xFFFFFFFF80600000; phys_to_table/zero_page eliminated from vmm.c; identity map still active (teardown Phase 7) |
```

- [ ] **Step 2: Add Phase 6 forward-looking constraints**

After the Phase 5 forward-looking constraints section, add:

```markdown
### Phase 6 forward-looking constraints

**Identity map still active.** Phase 6 eliminated `phys_to_table` from `vmm.c`
but TCBs and kernel stacks in `sched.c` and `proc.c` still cast PMM physical
addresses to pointers via the identity map. Run the following to get a complete
list before starting Phase 7:
```bash
grep -rn '(uint64_t \*)(uintptr_t)\|(uint8_t \*)(uintptr_t)\|(void \*)(uintptr_t)' kernel/
```
Known sites: `sched_spawn` (TCB + stack), `proc_spawn` (PCB + kernel stack).
Phase 7 must migrate these to a higher-half slab or kernel allocator, then
remove the `[0..4MB) → [0..4MB)` identity entries from `pd_lo` and flush TLB.

**Single window slot.** `s_window_pt[512]` has 511 unused entries. Phase 7+
work involving concurrent kernel threads or DMA mappings may require additional
slots.
```

- [ ] **Step 3: Update the "Last updated" timestamp**

Find the most recent `*Last updated:` line and add:
```
*Last updated: 2026-03-20 — Phase 6 complete, make test GREEN. Mapped-window allocator live; phys_to_table/zero_page eliminated from vmm.c.*
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
git commit -m "docs: Phase 6 complete — update CLAUDE.md build status and forward-looking constraints"
```
