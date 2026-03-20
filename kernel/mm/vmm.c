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

/*
 * zero_page — zero all 512 entries of a physical page.
 *
 * CONSTRAINT: phys must be < 4MB (within the identity-mapped window).
 * This cast is valid only while the [0..4MB) identity mapping is active.
 * Phase 4 must provide a mapped-window allocator before tearing down
 * the identity map. Tearing down identity first causes a fault that
 * cannot be debugged.
 */
static void
zero_page(uint64_t phys)
{
    uint64_t *p = (uint64_t *)(uintptr_t)phys;
    int i;
    for (i = 0; i < 512; i++)
        p[i] = 0;
}

/*
 * alloc_table — allocate a page-table page from the PMM and zero it.
 * Panics if the PMM is exhausted (returns 0).
 */
static uint64_t
alloc_table(void)
{
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) {
        printk("[VMM] FAIL: out of memory allocating page table\n");
        for (;;) {}
    }
    zero_page(phys);
    return phys;
}

/*
 * phys_to_table — cast a physical page-table address to a uint64_t pointer.
 * Valid while the identity mapping [0..4MB) is active.
 */
static uint64_t *
phys_to_table(uint64_t phys)
{
    return (uint64_t *)(uintptr_t)phys;
}

/*
 * ensure_table — if parent[idx] has no present child table, allocate one.
 * Returns the physical address of the child table.
 */
static uint64_t
ensure_table(uint64_t *parent, uint64_t idx)
{
    if (!(parent[idx] & VMM_FLAG_PRESENT)) {
        uint64_t child = alloc_table();
        parent[idx] = child | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
    }
    return PTE_ADDR(parent[idx]);
}

void
vmm_init(void)
{
    /* Allocate the five initial page tables. All allocations happen before
     * arch_vmm_load_pml4() is called, so zero_page() is safe: the identity
     * window [0..4MB) is still the active mapping at this point. */
    uint64_t pml4_phys    = alloc_table();
    uint64_t pdpt_lo_phys = alloc_table();
    uint64_t pd_lo_phys   = alloc_table();
    uint64_t pdpt_hi_phys = alloc_table();
    uint64_t pd_hi_phys   = alloc_table();

    uint64_t *pml4    = phys_to_table(pml4_phys);
    uint64_t *pdpt_lo = phys_to_table(pdpt_lo_phys);
    uint64_t *pd_lo   = phys_to_table(pd_lo_phys);
    uint64_t *pdpt_hi = phys_to_table(pdpt_hi_phys);
    uint64_t *pd_hi   = phys_to_table(pd_hi_phys);

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

    s_pml4_phys = pml4_phys;
    arch_vmm_load_pml4(pml4_phys);

    printk("[VMM] OK: kernel mapped to 0xFFFFFFFF80000000\n");
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

    uint64_t *pml4 = phys_to_table(s_pml4_phys);
    uint64_t pdpt_phys = ensure_table(pml4, pml4_idx);
    uint64_t *pdpt = phys_to_table(pdpt_phys);
    uint64_t pd_phys = ensure_table(pdpt, pdpt_idx);
    uint64_t *pd = phys_to_table(pd_phys);
    uint64_t pt_phys = ensure_table(pd, pd_idx);
    uint64_t *pt = phys_to_table(pt_phys);

    if (pt[pt_idx] & VMM_FLAG_PRESENT) {
        printk("[VMM] FAIL: vmm_map_page double-map\n");
        for (;;) {}
    }

    pt[pt_idx] = phys | flags | VMM_FLAG_PRESENT;
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

    uint64_t *pml4 = phys_to_table(s_pml4_phys);
    if (!(pml4[pml4_idx] & VMM_FLAG_PRESENT)) {
        printk("[VMM] FAIL: vmm_unmap_page not mapped (pml4)\n");
        for (;;) {}
    }
    uint64_t *pdpt = phys_to_table(PTE_ADDR(pml4[pml4_idx]));
    if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT)) {
        printk("[VMM] FAIL: vmm_unmap_page not mapped (pdpt)\n");
        for (;;) {}
    }
    uint64_t *pd = phys_to_table(PTE_ADDR(pdpt[pdpt_idx]));
    if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) {
        printk("[VMM] FAIL: vmm_unmap_page not mapped (pd)\n");
        for (;;) {}
    }
    if (pd[pd_idx] & (1UL << 7)) {
        printk("[VMM] FAIL: vmm_unmap_page called on huge-page-backed address\n");
        for (;;) {}
    }
    uint64_t *pt = phys_to_table(PTE_ADDR(pd[pd_idx]));
    if (!(pt[pt_idx] & VMM_FLAG_PRESENT)) {
        printk("[VMM] FAIL: vmm_unmap_page not mapped (pt)\n");
        for (;;) {}
    }

    pt[pt_idx] = 0;
    arch_vmm_invlpg(virt);
}

/*
 * ensure_table_user — like ensure_table but sets USER|PRESENT|WRITABLE
 * on newly created intermediate entries.
 * CRITICAL: ALL intermediate entries in a user page-table walk must have
 * VMM_FLAG_USER set. The MMU checks the USER bit at every level (PML4e,
 * PDPTe, PDe). A missing USER bit on any ancestor causes a ring-3 #PF
 * even if the leaf PTE is correct.
 */
static uint64_t
ensure_table_user(uint64_t *parent, uint64_t idx)
{
    if (!(parent[idx] & VMM_FLAG_PRESENT)) {
        uint64_t child = alloc_table();
        parent[idx] = child | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;
    }
    return PTE_ADDR(parent[idx]);
}

uint64_t
vmm_create_user_pml4(void)
{
    uint64_t new_pml4_phys = alloc_table();   /* zeroed by alloc_table */

    /* Copy kernel high entries [256..511] from master PML4.
     * This makes the kernel higher-half accessible in every user process's
     * address space, so syscall handlers can execute after SYSCALL without
     * a CR3 switch. */
    uint64_t *master = phys_to_table(s_pml4_phys);
    uint64_t *newpml = phys_to_table(new_pml4_phys);
    int i;
    for (i = 256; i < 512; i++)
        newpml[i] = master[i];

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

    uint64_t *pml4    = phys_to_table(pml4_phys);
    uint64_t pdpt_p   = ensure_table_user(pml4, pml4_idx);
    uint64_t *pdpt    = phys_to_table(pdpt_p);
    uint64_t pd_p     = ensure_table_user(pdpt, pdpt_idx);
    uint64_t *pd      = phys_to_table(pd_p);
    uint64_t pt_p     = ensure_table_user(pd, pd_idx);
    uint64_t *pt      = phys_to_table(pt_p);

    if (pt[pt_idx] & VMM_FLAG_PRESENT) {
        printk("[VMM] FAIL: vmm_map_user_page double-map\n");
        for (;;) {}
    }

    pt[pt_idx] = phys | flags | VMM_FLAG_PRESENT;
}

void
vmm_switch_to(uint64_t pml4_phys)
{
    arch_vmm_load_pml4(pml4_phys);
}
