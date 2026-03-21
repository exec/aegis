#include "vmm.h"
#include "arch.h"     /* arch_vmm_load_pml4, arch_vmm_invlpg */
#include "pmm.h"
#include "printk.h"
#include <stdint.h>
#include <stddef.h>

#define VMM_PAGE_SIZE   4096UL
#define VMM_PAGE_MASK   (~(VMM_PAGE_SIZE - 1))
#define PTE_ADDR(e)     ((e) & 0x000FFFFFFFFFF000UL)

/* Physical address of the active PML4 table. */
static uint64_t s_pml4_phys;

/* Mapped-window allocator (Phase 6).
 * A single virtual address whose PTE is permanently allocated in BSS.
 * vmm_window_map(phys) installs phys into the PTE and flushes TLB.
 * vmm_window_unmap() clears the PTE and flushes TLB.
 * The window is non-reentrant: never hold it across any call that may
 * itself call vmm_window_map (e.g. alloc_table). */
#define VMM_WINDOW_VA (ARCH_KERNEL_VIRT_BASE + 0x600000UL)

/* s_window_pt must be 4KB-aligned: pd_hi[3] stores its physical address as a
 * page-table pointer.  An unaligned address would set the PS bit (bit 7) in
 * the PDE, causing the CPU to interpret it as a huge-page entry with reserved
 * bits set — producing a #PF RSVD fault on first window access. */
static uint64_t           s_window_pt[512] __attribute__((aligned(4096))); /* BSS — PT for window range */
static volatile uint64_t *s_window_pte;     /* → s_window_pt[0], set at init   *
                                             * volatile: prevents the compiler  *
                                             * from caching the PTE value; each *
                                             * write must reach memory before   *
                                             * the __asm__ volatile invlpg.     */

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

/*
 * alloc_table — allocate a page-table page from the PMM and zero it.
 * Uses vmm_window_map/unmap to zero the page without the identity map.
 * Panics if the PMM is exhausted.
 *
 * Requires the window allocator to be active (s_window_pte != NULL).
 * vmm_init uses alloc_table_early() instead for the five bootstrap tables.
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

/*
 * alloc_table_early — allocate and zero a page-table page using the identity
 * map.  Valid only during vmm_init(), before arch_vmm_load_pml4() switches
 * to the new PML4 and before the window allocator is wired up.
 * After vmm_init() returns, call alloc_table() instead.
 */
static uint64_t
alloc_table_early(void)
{
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) {
        printk("[VMM] FAIL: out of memory allocating page table\n");
        for (;;) {}
    }
    /* SAFETY: identity map [0..4MB) is active; phys is within that range
     * because the PMM starts above _kernel_end which is well below 4MB. */
    uint64_t *p = (uint64_t *)(uintptr_t)phys;
    int i;
    for (i = 0; i < 512; i++)
        p[i] = 0;
    return phys;
}

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

void
vmm_init(void)
{
    /* Allocate the five initial page tables using the identity-map-based
     * early allocator.  The window allocator is not yet active here.
     * All five PMM pages are below 4MB so the identity cast is safe. */
    uint64_t pml4_phys    = alloc_table_early();
    uint64_t pdpt_lo_phys = alloc_table_early();
    uint64_t pd_lo_phys   = alloc_table_early();
    uint64_t pdpt_hi_phys = alloc_table_early();
    uint64_t pd_hi_phys   = alloc_table_early();

    /* SAFETY: identity map [0..4MB) is active for all five tables. */
    uint64_t *pml4    = (uint64_t *)(uintptr_t)pml4_phys;
    uint64_t *pdpt_lo = (uint64_t *)(uintptr_t)pdpt_lo_phys;
    uint64_t *pd_lo   = (uint64_t *)(uintptr_t)pd_lo_phys;
    uint64_t *pdpt_hi = (uint64_t *)(uintptr_t)pdpt_hi_phys;
    uint64_t *pd_hi   = (uint64_t *)(uintptr_t)pd_hi_phys;

    /* Identity map: VA [0..4MB) → PA [0..4MB) via PML4[0].
     * PML4[0] → pdpt_lo[0] → pd_lo with two 2MB huge pages. */
    pml4[0]    = pdpt_lo_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
    pdpt_lo[0] = pd_lo_phys   | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
    pd_lo[0]   = 0x000000UL   | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | (1UL << 7);
    pd_lo[1]   = 0x200000UL   | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | (1UL << 7);

    /* Higher-half map: VA [KERN_VMA..KERN_VMA+4MB) → PA [0..4MB).
     * KERN_VMA = 0xFFFFFFFF80000000 → PML4[511], PDPT[510].
     * PML4[511] → pdpt_hi[510] → pd_hi with two 2MB huge pages. */
    pml4[511]    = pdpt_hi_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
    pdpt_hi[510] = pd_hi_phys   | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
    pd_hi[0]     = 0x000000UL   | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | (1UL << 7);
    pd_hi[1]     = 0x200000UL   | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | (1UL << 7);

    /* Install the mapped-window PT into pd_hi[3].
     * pd_hi is a local pointer (identity-cast of pd_hi_phys) in scope here.
     * pd_hi[0] and pd_hi[1] are the two 2MB kernel huge pages.
     * pd_hi[2] is used by KSTACK_VA at runtime — do not touch.
     * pd_hi[3] covers 0xFFFFFFFF80600000 (VMM_WINDOW_VA) — currently NULL. */
    {
        /* Physical address of s_window_pt: PA = VA - KERN_VMA.
         * All higher-half symbols have PA = VA - ARCH_KERNEL_VIRT_BASE
         * (the linker AT() directive sets LMA = VMA - KERN_VMA).
         * Adding ARCH_KERNEL_PHYS_BASE would be wrong: PHYS_BASE is just
         * where the first section starts, not an additive offset. */
        uint64_t win_phys = (uint64_t)(uintptr_t)s_window_pt
                            - ARCH_KERNEL_VIRT_BASE;
        pd_hi[3]     = win_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
        s_window_pte = &s_window_pt[0];
    }

    s_pml4_phys = pml4_phys;
    arch_vmm_load_pml4(pml4_phys);

    printk("[VMM] OK: kernel mapped to 0xFFFFFFFF80000000\n");
    printk("[VMM] OK: mapped-window allocator active\n");
}

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
        vmm_window_unmap();
        printk("[VMM] FAIL: vmm_map_page double-map\n");
        for (;;) {}
    }
    pt[pt_idx] = phys | flags | VMM_FLAG_PRESENT;
    vmm_window_unmap();
}

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

    /* Walk-overwrite pattern: each vmm_window_map call overwrites the PTE
     * from the previous level without an intervening unmap. Only one
     * vmm_window_unmap call at the very end. This halves the invlpg count
     * (4 maps + 1 unmap = 5 vs. 4 maps + 4 unmaps = 8) and eliminates the
     * window between unmap and remap where a stale TLB entry could yield
     * a silent wrong read if any interleaved code runs between them. */
    uint64_t *pml4  = vmm_window_map(s_pml4_phys);
    uint64_t  pml4e = pml4[pml4_idx];
    if (!(pml4e & VMM_FLAG_PRESENT)) {
        vmm_window_unmap();
        printk("[VMM] FAIL: vmm_unmap_page not mapped (pml4)\n");
        for (;;) {}
    }

    uint64_t *pdpt  = vmm_window_map(PTE_ADDR(pml4e));  /* overwrites PTE */
    uint64_t  pdpte = pdpt[pdpt_idx];
    if (!(pdpte & VMM_FLAG_PRESENT)) {
        vmm_window_unmap();
        printk("[VMM] FAIL: vmm_unmap_page not mapped (pdpt)\n");
        for (;;) {}
    }

    uint64_t *pd  = vmm_window_map(PTE_ADDR(pdpte));    /* overwrites PTE */
    uint64_t  pde = pd[pd_idx];
    if (!(pde & VMM_FLAG_PRESENT)) {
        vmm_window_unmap();
        printk("[VMM] FAIL: vmm_unmap_page not mapped (pd)\n");
        for (;;) {}
    }
    if (pde & (1UL << 7)) {
        vmm_window_unmap();
        printk("[VMM] FAIL: vmm_unmap_page called on huge-page-backed address\n");
        for (;;) {}
    }

    uint64_t *pt  = vmm_window_map(PTE_ADDR(pde));      /* overwrites PTE */
    uint64_t  pte = pt[pt_idx];
    if (!(pte & VMM_FLAG_PRESENT)) {
        vmm_window_unmap();
        printk("[VMM] FAIL: vmm_unmap_page not mapped (pt)\n");
        for (;;) {}
    }
    pt[pt_idx] = 0;
    vmm_window_unmap();          /* single unmap at the end of the walk */
    arch_vmm_invlpg(virt);
}

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
    if (pde & (1UL << 7)) {
        vmm_window_unmap();
        printk("[VMM] FAIL: vmm_phys_of called on huge-page-backed address\n");
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

uint64_t
vmm_create_user_pml4(void)
{
    uint64_t new_pml4_phys = alloc_table();   /* zeroed by alloc_table */

    /* Copy kernel high entries [256..511] from master PML4.
     * This makes the kernel higher-half accessible in every user process's
     * address space, so syscall handlers can execute after SYSCALL without
     * a CR3 switch.
     * Two window map/unmap pairs: first to read from master, then to write
     * to new PML4.  A local array holds the 256 entries between the two
     * maps to avoid holding the window across a second map call. */
    uint64_t tmp[256];
    uint64_t *master = vmm_window_map(s_pml4_phys);
    int i;
    for (i = 0; i < 256; i++)
        tmp[i] = master[256 + i];
    vmm_window_unmap();

    uint64_t *newpml = vmm_window_map(new_pml4_phys);
    for (i = 0; i < 256; i++)
        newpml[256 + i] = tmp[i];
    vmm_window_unmap();

    return new_pml4_phys;
}

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

    uint64_t pdpt_phys = ensure_table_phys(pml4_phys,  pml4_idx, VMM_FLAG_USER);
    uint64_t pd_phys   = ensure_table_phys(pdpt_phys,  pdpt_idx, VMM_FLAG_USER);
    uint64_t pt_phys   = ensure_table_phys(pd_phys,    pd_idx,   VMM_FLAG_USER);

    uint64_t *pt = vmm_window_map(pt_phys);
    if (pt[pt_idx] & VMM_FLAG_PRESENT) {
        vmm_window_unmap();
        printk("[VMM] FAIL: vmm_map_user_page double-map\n");
        for (;;) {}
    }
    pt[pt_idx] = phys | flags | VMM_FLAG_PRESENT;
    vmm_window_unmap();
}

void
vmm_switch_to(uint64_t pml4_phys)
{
    arch_vmm_load_pml4(pml4_phys);
}

uint64_t
vmm_get_master_pml4(void)
{
    return s_pml4_phys;
}
