/*
 * vmm_arm64.c — ARM64 virtual memory manager.
 *
 * Implements the vmm.h API for ARM64. The boot.S MMU setup provides an
 * identity map via 1GB block descriptors. vmm_init() transitions to a
 * proper 4KB-granule page table that maps the kernel region and sets up
 * the window allocator for dynamic page table manipulation.
 *
 * ARM64 4KB granule, 48-bit VA (TTBR0):
 *   L0: bits [47:39] — 512 entries, 512GB each
 *   L1: bits [38:30] — 512 entries, 1GB each
 *   L2: bits [29:21] — 512 entries, 2MB each
 *   L3: bits [20:12] — 512 entries, 4KB each (page descriptors)
 *
 * Page table entry format:
 *   [0]     valid
 *   [1]     1=table (L0-L2) or 1=page (L3), 0=block (L1-L2)
 *   [4:2]   AttrIndx (MAIR index)
 *   [7:6]   AP (access permissions)
 *   [9:8]   SH (shareability)
 *   [10]    AF (access flag)
 *   [47:12] output address
 *   [54]    XN (execute never)
 */

#include "vmm.h"
#include "arch.h"
#include "pmm.h"
#include "printk.h"
#include <stdint.h>

/* ARM64 PTE bits */
#define PTE_VALID       (1UL << 0)
#define PTE_TABLE       (1UL << 1)
#define PTE_PAGE        (1UL << 1)
#define PTE_AF          (1UL << 10)
#define PTE_SH_INNER    (3UL << 8)
#define PTE_AP_RW_EL1   (0UL << 6)
#define PTE_AP_RW_ALL   (1UL << 6)
#define PTE_ATTR_NORM   (0UL << 2)
#define PTE_ATTR_DEV    (1UL << 2)
#define PTE_XN          (1UL << 54)

#define PTE_ADDR(e)     ((e) & 0x0000FFFFFFFFF000UL)
#define PAGE_SIZE_      4096UL

/* Master PML4 (L0 table) physical address */
static uint64_t s_pml4_phys;

/* Mapped-window allocator — same concept as x86, one virtual page whose
 * PTE can be rewritten to map any physical page temporarily.
 *
 * On ARM64 identity-mapped, VMM_WINDOW_VA is a physical address.
 * We allocate a 4KB-granule L3 page table and dedicate entry [0] and [1]
 * as the two window slots. */
static uint64_t s_window_pt[512] __attribute__((aligned(4096)));
static volatile uint64_t *s_window_pte;

/* Window VA: we pick an address in the identity-mapped region that doesn't
 * conflict with kernel code/data. Use a fixed VA past the kernel. */
/* Window at L2[32] = +64MB, just past the 32 block-mapped entries. */
#define VMM_WINDOW_VA (ARCH_KERNEL_VIRT_BASE + 0x4000000UL)

/* ── Window allocator ──────────────────────────────────────────────── */

static void *
vmm_window_map(uint64_t phys)
{
    *s_window_pte = phys | PTE_VALID | PTE_PAGE | PTE_AF |
                    PTE_SH_INNER | PTE_ATTR_NORM | PTE_AP_RW_EL1;
    arch_vmm_invlpg(VMM_WINDOW_VA);
    return (void *)VMM_WINDOW_VA;
}

static void
vmm_window_unmap(void)
{
    *s_window_pte = 0;
    arch_vmm_invlpg(VMM_WINDOW_VA);
}

/* ── Table allocation ──────────────────────────────────────────────── */

/* Early alloc: identity-mapped, direct physical pointer cast. */
static uint64_t
alloc_table_early(void)
{
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) {
        printk("[VMM] FAIL: out of memory\n");
        for (;;) arch_halt();
    }
    uint64_t *p = (uint64_t *)(uintptr_t)phys;
    for (int i = 0; i < 512; i++)
        p[i] = 0;
    return phys;
}

/* Post-init alloc: uses window to zero the page. */
static uint64_t
alloc_table(void)
{
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) {
        printk("[VMM] FAIL: out of memory\n");
        for (;;) arch_halt();
    }
    uint64_t *t = vmm_window_map(phys);
    for (int i = 0; i < 512; i++)
        t[i] = 0;
    vmm_window_unmap();
    return phys;
}

/* ── Table walk helpers ────────────────────────────────────────────── */

/* Translate abstract VMM_FLAG_* to ARM64 PTE bits for a leaf (L3 page). */
static uint64_t
flags_to_pte(uint64_t flags)
{
    uint64_t pte = 0;
    if (flags & VMM_FLAG_PRESENT)
        pte |= PTE_VALID | PTE_PAGE | PTE_AF | PTE_SH_INNER | PTE_ATTR_NORM;
    if (flags & VMM_FLAG_USER)
        pte |= PTE_AP_RW_ALL;
    if (flags & VMM_FLAG_NX)
        pte |= PTE_XN;
    if (flags & (VMM_FLAG_WC | VMM_FLAG_UCMINUS)) {
        pte &= ~(7UL << 2);
        pte |= PTE_ATTR_DEV;
        pte &= ~PTE_SH_INNER;
    }
    return pte;
}

/* Intermediate (non-leaf) table entry: valid + table descriptor. */
static uint64_t
table_entry(uint64_t child_phys)
{
    /* ARM64 table descriptors: valid + table bits only.
     * APTable/UXNTable/PXNTable default to 0 = no restriction. */
    return child_phys | PTE_VALID | PTE_TABLE;
}

/* Ensure a child table exists at parent[idx]. Returns child phys addr. */
static uint64_t
ensure_table(uint64_t parent_phys, uint64_t idx)
{
    uint64_t *parent = vmm_window_map(parent_phys);
    uint64_t entry = parent[idx];
    vmm_window_unmap();

    if (!(entry & PTE_VALID)) {
        uint64_t child = alloc_table();
        parent = vmm_window_map(parent_phys);
        parent[idx] = table_entry(child);
        vmm_window_unmap();
        return child;
    }
    return PTE_ADDR(entry);
}

/* ── Public API ────────────────────────────────────────────────────── */

void
vmm_init(void)
{
    /*
     * Build a proper 4KB-granule page table that identity-maps all RAM
     * and the device region (0x00000000-0x40000000).
     *
     * Layout:
     *   L0[0] → L1 table
     *   L1[0] → L2 table (device region 0x00000000-0x3FFFFFFF)
     *     L2[0..511] → 2MB block descriptors (device memory)
     *   L1[1] → L2 table (RAM 0x40000000-0x7FFFFFFF)
     *     L2[0..63] → 2MB block descriptors (128MB normal memory)
     *     L2[window_idx] → s_window_pt (4KB page table for window allocator)
     */
    uint64_t l0_phys = alloc_table_early();
    uint64_t l1_phys = alloc_table_early();
    uint64_t l2_dev_phys = alloc_table_early();
    uint64_t l2_ram_phys = alloc_table_early();

    uint64_t *l0     = (uint64_t *)(uintptr_t)l0_phys;
    uint64_t *l1     = (uint64_t *)(uintptr_t)l1_phys;
    uint64_t *l2_dev = (uint64_t *)(uintptr_t)l2_dev_phys;
    uint64_t *l2_ram = (uint64_t *)(uintptr_t)l2_ram_phys;

    /* L0[0] → L1 */
    l0[0] = l1_phys | PTE_VALID | PTE_TABLE;

    /* L1[0] → L2 device, L1[1] → L2 RAM */
    l1[0] = l2_dev_phys | PTE_VALID | PTE_TABLE;
    l1[1] = l2_ram_phys | PTE_VALID | PTE_TABLE;

    /* L2 device: 512 × 2MB block descriptors for 0x00000000-0x3FFFFFFF
     * Block descriptor at L2: bit[0]=valid, bit[1]=0 (block, not table) */
    for (int i = 0; i < 512; i++) {
        l2_dev[i] = ((uint64_t)i * 0x200000UL) |
                     PTE_VALID | /* block: bit[1]=0 */ PTE_AF |
                     PTE_ATTR_DEV | PTE_AP_RW_EL1 | PTE_XN;
    }

    /* L2 RAM: map first 5 × 2MB blocks (0x40000000 - 0x409FFFFF, 10MB)
     * covering kernel image, BSS, boot stack, and early allocations.
     * L2[4] is replaced by the window allocator table below.
     * L2[5+] are left unmapped — KVA creates L3 tables on demand via
     * vmm_map_page → ensure_table. PMM still knows about all 128MB. */
    /* Map first 32 × 2MB = 64MB as block descriptors.
     * Remaining RAM (64MB+) is available to KVA via L3 page tables. */
    for (int i = 0; i < 32; i++) {
        l2_ram[i] = (0x40000000UL + (uint64_t)i * 0x200000UL) |
                     PTE_VALID | /* block: bit[1]=0 */ PTE_AF |
                     PTE_SH_INNER | PTE_ATTR_NORM | PTE_AP_RW_EL1;
    }

    /* Wire the window allocator: put s_window_pt as an L2 entry covering
     * VMM_WINDOW_VA. VMM_WINDOW_VA = 0x40800000, which is L1[1], L2[4]
     * (offset 0x800000 / 0x200000 = 4). We need this to be a TABLE entry
     * pointing to our L3 page table (s_window_pt), not a block. */
    {
        /* First, unmap the 2MB block at L2[4] (if it was mapped) so we
         * can replace it with a table entry pointing to s_window_pt. */
        uint64_t win_l2_idx = (VMM_WINDOW_VA - ARCH_KERNEL_VIRT_BASE) / 0x200000UL;
        /* s_window_pt is a BSS variable at high VA; convert to PA for page table */
        uint64_t win_pt_phys = (uint64_t)(uintptr_t)s_window_pt - KERN_VA_OFFSET;

        /* Zero the window PT */
        for (int i = 0; i < 512; i++)
            s_window_pt[i] = 0;

        /* Replace the block mapping at L2[win_l2_idx] with a table entry */
        l2_ram[win_l2_idx] = win_pt_phys | PTE_VALID | PTE_TABLE;

        s_window_pte = &s_window_pt[0];
    }

    s_pml4_phys = l0_phys;
    arch_vmm_load_pml4(l0_phys);

    printk("[VMM] OK: ARM64 4KB-granule page tables active\n");
    printk("[VMM] OK: mapped-window allocator active\n");
}

uint64_t
vmm_get_master_pml4(void)
{
    return s_pml4_phys;
}

void
vmm_switch_to(uint64_t pml4_phys)
{
    /* Switch user address space (TTBR0). */
    extern void arch_vmm_load_user_ttbr0(uint64_t phys);
    arch_vmm_load_user_ttbr0(pml4_phys);
}

void
vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t l0_idx = (virt >> 39) & 0x1FF;
    uint64_t l1_idx = (virt >> 30) & 0x1FF;
    uint64_t l2_idx = (virt >> 21) & 0x1FF;
    uint64_t l3_idx = (virt >> 12) & 0x1FF;

    uint64_t l1_phys = ensure_table(s_pml4_phys, l0_idx);
    uint64_t l2_phys = ensure_table(l1_phys, l1_idx);
    uint64_t l3_phys = ensure_table(l2_phys, l2_idx);

    uint64_t *l3 = vmm_window_map(l3_phys);
    l3[l3_idx] = phys | flags_to_pte(flags | VMM_FLAG_PRESENT);
    vmm_window_unmap();
    arch_vmm_invlpg(virt);
}

void
vmm_unmap_page(uint64_t virt)
{
    uint64_t l0_idx = (virt >> 39) & 0x1FF;
    uint64_t l1_idx = (virt >> 30) & 0x1FF;
    uint64_t l2_idx = (virt >> 21) & 0x1FF;
    uint64_t l3_idx = (virt >> 12) & 0x1FF;

    /* Walk to L3 using window */
    uint64_t *l0 = vmm_window_map(s_pml4_phys);
    uint64_t l0e = l0[l0_idx];
    if (!(l0e & PTE_VALID)) { vmm_window_unmap(); return; }

    *s_window_pte = PTE_ADDR(l0e) | PTE_VALID | PTE_PAGE | PTE_AF |
                    PTE_SH_INNER | PTE_ATTR_NORM;
    arch_vmm_invlpg(VMM_WINDOW_VA);
    uint64_t *l1 = (uint64_t *)VMM_WINDOW_VA;
    uint64_t l1e = l1[l1_idx];
    if (!(l1e & PTE_VALID)) { vmm_window_unmap(); return; }

    *s_window_pte = PTE_ADDR(l1e) | PTE_VALID | PTE_PAGE | PTE_AF |
                    PTE_SH_INNER | PTE_ATTR_NORM;
    arch_vmm_invlpg(VMM_WINDOW_VA);
    uint64_t *l2 = (uint64_t *)VMM_WINDOW_VA;
    uint64_t l2e = l2[l2_idx];
    if (!(l2e & PTE_VALID)) { vmm_window_unmap(); return; }

    *s_window_pte = PTE_ADDR(l2e) | PTE_VALID | PTE_PAGE | PTE_AF |
                    PTE_SH_INNER | PTE_ATTR_NORM;
    arch_vmm_invlpg(VMM_WINDOW_VA);
    uint64_t *l3 = (uint64_t *)VMM_WINDOW_VA;
    l3[l3_idx] = 0;
    vmm_window_unmap();
    arch_vmm_invlpg(virt);
}

void
vmm_map_user_page(uint64_t pml4_phys, uint64_t virt,
                  uint64_t phys, uint64_t flags)
{
    /* Map user pages into the per-process PML4 (TTBR0).
     * User addresses are in the lower half (bit 55 = 0). */
    uint64_t l0_idx = (virt >> 39) & 0x1FF;
    uint64_t l1_idx = (virt >> 30) & 0x1FF;
    uint64_t l2_idx = (virt >> 21) & 0x1FF;
    uint64_t l3_idx = (virt >> 12) & 0x1FF;

    uint64_t l1_phys = ensure_table(pml4_phys, l0_idx);
    uint64_t l2_phys = ensure_table(l1_phys, l1_idx);
    uint64_t l3_phys = ensure_table(l2_phys, l2_idx);

    uint64_t *l3 = vmm_window_map(l3_phys);
    l3[l3_idx] = phys | flags_to_pte(flags | VMM_FLAG_PRESENT);
    vmm_window_unmap();
    arch_vmm_invlpg(virt);
}

uint64_t
vmm_create_user_pml4(void)
{
    /* User PML4 for TTBR0 (lower half). Starts empty — user pages are
     * mapped via vmm_map_user_page → ensure_table. Kernel is in TTBR1
     * (upper half), so no kernel entries needed in user PML4. */
    return alloc_table();
}

uint64_t
vmm_phys_of(uint64_t virt)
{
    uint64_t l0_idx = (virt >> 39) & 0x1FF;
    uint64_t l1_idx = (virt >> 30) & 0x1FF;
    uint64_t l2_idx = (virt >> 21) & 0x1FF;
    uint64_t l3_idx = (virt >> 12) & 0x1FF;

    uint64_t *tbl = vmm_window_map(s_pml4_phys);
    uint64_t e = tbl[l0_idx];
    if (!(e & PTE_VALID)) { vmm_window_unmap(); return 0; }

    *s_window_pte = PTE_ADDR(e) | PTE_VALID | PTE_PAGE | PTE_AF |
                    PTE_SH_INNER | PTE_ATTR_NORM;
    arch_vmm_invlpg(VMM_WINDOW_VA);
    tbl = (uint64_t *)VMM_WINDOW_VA;
    e = tbl[l1_idx];
    if (!(e & PTE_VALID)) { vmm_window_unmap(); return 0; }
    /* Check if L1 block (bit[1]=0) */
    if (!(e & PTE_TABLE))
        return PTE_ADDR(e) | (virt & 0x3FFFFFFFUL);

    *s_window_pte = PTE_ADDR(e) | PTE_VALID | PTE_PAGE | PTE_AF |
                    PTE_SH_INNER | PTE_ATTR_NORM;
    arch_vmm_invlpg(VMM_WINDOW_VA);
    e = tbl[l2_idx];
    if (!(e & PTE_VALID)) { vmm_window_unmap(); return 0; }
    if (!(e & PTE_TABLE))
        return PTE_ADDR(e) | (virt & 0x1FFFFFUL);

    *s_window_pte = PTE_ADDR(e) | PTE_VALID | PTE_PAGE | PTE_AF |
                    PTE_SH_INNER | PTE_ATTR_NORM;
    arch_vmm_invlpg(VMM_WINDOW_VA);
    e = tbl[l3_idx];
    vmm_window_unmap();
    if (!(e & PTE_VALID)) return 0;
    return PTE_ADDR(e) | (virt & 0xFFFUL);
}

/* Stubs for functions not yet needed on ARM64 boot stage */
void vmm_teardown_identity(void) {
    printk("[VMM] OK: identity map retained (ARM64)\n");
}

void vmm_free_user_pml4(uint64_t pml4_phys) { (void)pml4_phys; }
uint64_t vmm_phys_of_user(uint64_t pml4_phys, uint64_t virt) {
    (void)pml4_phys; (void)virt; return 0;
}
void vmm_unmap_user_page(uint64_t pml4_phys, uint64_t virt) {
    (void)pml4_phys; (void)virt;
}
void vmm_zero_page(uint64_t phys) {
    uint64_t *p = vmm_window_map(phys);
    for (int i = 0; i < 512; i++) p[i] = 0;
    vmm_window_unmap();
}
int vmm_copy_user_pages(uint64_t src, uint64_t dst) {
    (void)src; (void)dst; return -1;
}
void vmm_free_user_pages(uint64_t pml4_phys) { (void)pml4_phys; }
int vmm_write_user_bytes(uint64_t pml4_phys, uint64_t va,
                         const void *src, uint64_t len) {
    (void)pml4_phys; (void)va; (void)src; (void)len; return -1;
}
int vmm_write_user_u64(uint64_t pml4_phys, uint64_t va, uint64_t val) {
    (void)pml4_phys; (void)va; (void)val; return -1;
}
