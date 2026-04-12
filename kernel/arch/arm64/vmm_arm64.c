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
/* Window at L2[128] = +256MB, just past the 128 block-mapped entries. */
#define VMM_WINDOW_VA (ARCH_KERNEL_VIRT_BASE + 0x10000000UL)

/* ── Window allocator ──────────────────────────────────────────────── */

void *
vmm_window_map(uint64_t phys)
{
    *s_window_pte = phys | PTE_VALID | PTE_PAGE | PTE_AF |
                    PTE_SH_INNER | PTE_ATTR_NORM | PTE_AP_RW_EL1;
    arch_vmm_invlpg(VMM_WINDOW_VA);
    return (void *)VMM_WINDOW_VA;
}

void
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

/* Post-init alloc: zero via TTBR1 high VA. */
static uint64_t
alloc_table(void)
{
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) {
        printk("[VMM] FAIL: out of memory\n");
        for (;;) arch_halt();
    }
    uint64_t *t = (uint64_t *)(uintptr_t)(phys + KERN_VA_OFFSET);
    for (int i = 0; i < 512; i++)
        t[i] = 0;
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

/* Ensure a child table exists at parent[idx]. Returns child phys addr.
 * Uses TTBR1 high VA (PA + KERN_VA_OFFSET) instead of window to avoid
 * window conflicts during nested page table manipulation. */
static uint64_t
ensure_table(uint64_t parent_phys, uint64_t idx)
{
    uint64_t *parent = (uint64_t *)(uintptr_t)(parent_phys + KERN_VA_OFFSET);
    uint64_t entry = parent[idx];

    if (!(entry & PTE_VALID)) {
        uint64_t child = alloc_table();
        parent[idx] = table_entry(child);
        /* DSB + ISB: ensure the page table write is visible to the MMU
         * before any subsequent table walk or data access. */
        __asm__ volatile("dsb sy; isb" ::: "memory");
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
    /* Map 128 × 2MB = 256MB as block descriptors (covers -m 256M).
     * Window at L2[128], KVA at L2[129+]. Blocks cover all PMM range. */
    for (int i = 0; i < 128; i++) {
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

    uint64_t *l3 = (uint64_t *)(uintptr_t)(l3_phys + KERN_VA_OFFSET);
    l3[l3_idx] = phys | flags_to_pte(flags | VMM_FLAG_PRESENT);
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

    uint64_t *l3 = (uint64_t *)(uintptr_t)(l3_phys + KERN_VA_OFFSET);
    l3[l3_idx] = phys | flags_to_pte(flags | VMM_FLAG_PRESENT);
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
    /* Walk kernel page tables via TTBR1 high VA (no window). */
    uint64_t l0_idx = (virt >> 39) & 0x1FF;
    uint64_t l1_idx = (virt >> 30) & 0x1FF;
    uint64_t l2_idx = (virt >> 21) & 0x1FF;
    uint64_t l3_idx = (virt >> 12) & 0x1FF;

    uint64_t *l0 = (uint64_t *)(uintptr_t)(s_pml4_phys + KERN_VA_OFFSET);
    uint64_t e = l0[l0_idx];
    if (!(e & PTE_VALID)) return 0;

    uint64_t *l1 = (uint64_t *)(uintptr_t)(PTE_ADDR(e) + KERN_VA_OFFSET);
    e = l1[l1_idx];
    if (!(e & PTE_VALID)) return 0;
    if (!(e & PTE_TABLE))
        return PTE_ADDR(e) | (virt & 0x3FFFFFFFUL);

    uint64_t *l2 = (uint64_t *)(uintptr_t)(PTE_ADDR(e) + KERN_VA_OFFSET);
    e = l2[l2_idx];
    if (!(e & PTE_VALID)) return 0;
    if (!(e & PTE_TABLE))
        return PTE_ADDR(e) | (virt & 0x1FFFFFUL);

    uint64_t *l3 = (uint64_t *)(uintptr_t)(PTE_ADDR(e) + KERN_VA_OFFSET);
    e = l3[l3_idx];
    if (!(e & PTE_VALID)) return 0;
    return PTE_ADDR(e) | (virt & 0xFFFUL);
}

/* Stubs for functions not yet needed on ARM64 boot stage */
void vmm_teardown_identity(void) {
    printk("[VMM] OK: identity map retained (ARM64)\n");
}

void vmm_free_user_pml4(uint64_t pml4_phys) { (void)pml4_phys; }
uint64_t vmm_phys_of_user(uint64_t pml4_phys, uint64_t virt) {
    /* Walk user page tables via TTBR1 high VA (no window). */
    uint64_t l0_idx = (virt >> 39) & 0x1FF;
    uint64_t l1_idx = (virt >> 30) & 0x1FF;
    uint64_t l2_idx = (virt >> 21) & 0x1FF;
    uint64_t l3_idx = (virt >> 12) & 0x1FF;

    uint64_t *l0 = (uint64_t *)(uintptr_t)(pml4_phys + KERN_VA_OFFSET);
    uint64_t e = l0[l0_idx];
    if (!(e & PTE_VALID)) return 0;

    uint64_t *l1 = (uint64_t *)(uintptr_t)(PTE_ADDR(e) + KERN_VA_OFFSET);
    e = l1[l1_idx];
    if (!(e & PTE_VALID)) return 0;
    if (!(e & PTE_TABLE)) return PTE_ADDR(e) | (virt & 0x3FFFFFFFUL);

    uint64_t *l2 = (uint64_t *)(uintptr_t)(PTE_ADDR(e) + KERN_VA_OFFSET);
    e = l2[l2_idx];
    if (!(e & PTE_VALID)) return 0;
    if (!(e & PTE_TABLE)) return PTE_ADDR(e) | (virt & 0x1FFFFFUL);

    uint64_t *l3 = (uint64_t *)(uintptr_t)(PTE_ADDR(e) + KERN_VA_OFFSET);
    e = l3[l3_idx];
    if (!(e & PTE_VALID)) return 0;
    return PTE_ADDR(e) | (virt & 0xFFFUL);
}
void vmm_unmap_user_page(uint64_t pml4_phys, uint64_t virt) {
    (void)pml4_phys; (void)virt;
}
void vmm_zero_page(uint64_t phys) {
    uint64_t *p = (uint64_t *)(uintptr_t)(phys + KERN_VA_OFFSET);
    for (int i = 0; i < 512; i++) p[i] = 0;
}
int vmm_copy_user_pages(uint64_t src_pml4, uint64_t dst_pml4) {
    /* Walk src page tables via TTBR1 high VA (PA + KERN_VA_OFFSET).
     * TTBR0 may not have identity map during fork (scheduler switches it).
     * TTBR1 kern_l1[1] maps all RAM PA 0x40000000+ at high VA. */
    uint64_t i0, i1, i2, i3;

    /* Convert PA → kernel VA for page table access */
    #define PA_TO_KVA(pa) ((uint64_t *)(uintptr_t)((pa) + KERN_VA_OFFSET))

    uint64_t *l0 = PA_TO_KVA(src_pml4);

    for (i0 = 0; i0 < 512; i0++) {
        if (!(l0[i0] & PTE_VALID)) continue;
        uint64_t *l1 = PA_TO_KVA(PTE_ADDR(l0[i0]));

        for (i1 = 0; i1 < 512; i1++) {
            if (!(l1[i1] & PTE_VALID)) continue;
            if (!(l1[i1] & PTE_TABLE)) continue;
            uint64_t *l2 = PA_TO_KVA(PTE_ADDR(l1[i1]));

            for (i2 = 0; i2 < 512; i2++) {
                if (!(l2[i2] & PTE_VALID)) continue;
                if (!(l2[i2] & PTE_TABLE)) continue;
                uint64_t *l3 = PA_TO_KVA(PTE_ADDR(l2[i2]));

                for (i3 = 0; i3 < 512; i3++) {
                    if (!(l3[i3] & PTE_VALID)) continue;
                    uint64_t src_phys = PTE_ADDR(l3[i3]);

                    uint64_t new_phys = pmm_alloc_page();
                    if (!new_phys) return -1;

                    /* Copy 4KB via TTBR1 high VA */
                    __builtin_memcpy((void *)(uintptr_t)(new_phys + KERN_VA_OFFSET),
                                     (const void *)(uintptr_t)(src_phys + KERN_VA_OFFSET),
                                     4096);

                    uint64_t va = (i0 << 39) | (i1 << 30) | (i2 << 21) | (i3 << 12);
                    vmm_map_user_page(dst_pml4, va, new_phys,
                                      VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE);
                }
            }
        }
    }
    #undef PA_TO_KVA
    return 0;
}
/* vmm_phys_of_user_raw — like vmm_phys_of_user but returns the physical
 * address backing a leaf PTE even when the VALID bit has been cleared
 * (PROT_NONE pages). Used by sys_munmap to free frames of PROT_NONE pages
 * without leaking them.
 *
 * ARM64 semantics: we represent PROT_NONE by clearing bit 0 (VALID) on an
 * L3 page descriptor while keeping the output address in bits [47:12]
 * intact (see vmm_set_user_prot). The MMU ignores the entire entry when
 * VALID is clear, so this is safe. Here we walk down and return the leaf
 * PA for any non-zero L3 entry regardless of VALID.
 *
 * Walk is done via TTBR1 high VA (PA + KERN_VA_OFFSET), matching the
 * pattern used by vmm_phys_of_user. Intermediate levels (L0/L1/L2) must
 * still be VALID since a cleared intermediate descriptor means the entire
 * sub-tree is gone. */
uint64_t
vmm_phys_of_user_raw(uint64_t pml4_phys, uint64_t virt)
{
    uint64_t l0_idx = (virt >> 39) & 0x1FF;
    uint64_t l1_idx = (virt >> 30) & 0x1FF;
    uint64_t l2_idx = (virt >> 21) & 0x1FF;
    uint64_t l3_idx = (virt >> 12) & 0x1FF;

    uint64_t *l0 = (uint64_t *)(uintptr_t)(pml4_phys + KERN_VA_OFFSET);
    uint64_t e = l0[l0_idx];
    if (!(e & PTE_VALID)) return 0;

    uint64_t *l1 = (uint64_t *)(uintptr_t)(PTE_ADDR(e) + KERN_VA_OFFSET);
    e = l1[l1_idx];
    if (!(e & PTE_VALID)) return 0;
    /* L1 block descriptor (1GB block): return PA + offset. PROT_NONE is
     * not tracked on block mappings, so VALID must be set here. */
    if (!(e & PTE_TABLE)) return PTE_ADDR(e) | (virt & 0x3FFFFFFFUL);

    uint64_t *l2 = (uint64_t *)(uintptr_t)(PTE_ADDR(e) + KERN_VA_OFFSET);
    e = l2[l2_idx];
    if (!(e & PTE_VALID)) return 0;
    /* L2 block descriptor (2MB block): return PA + offset. */
    if (!(e & PTE_TABLE)) return PTE_ADDR(e) | (virt & 0x1FFFFFUL);

    uint64_t *l3 = (uint64_t *)(uintptr_t)(PTE_ADDR(e) + KERN_VA_OFFSET);
    e = l3[l3_idx];
    /* Raw: return PA for any non-zero leaf, even if VALID is clear. */
    return e ? (PTE_ADDR(e) | (virt & 0xFFFUL)) : 0;
}

/* vmm_set_user_prot — change the permissions of a single user L3 page.
 *
 * Walks pml4_phys (TTBR0 root) down to the L3 leaf and rewrites it to
 * match `flags` (VMM_FLAG_*). The output address is preserved.
 *
 *   flags == 0                → PROT_NONE: clear VALID bit, keep PA so
 *                               vmm_phys_of_user_raw can still find it.
 *   flags & VMM_FLAG_PRESENT  → build a fresh L3 descriptor via
 *                               flags_to_pte() (the same helper used by
 *                               vmm_map_user_page), OR the PA, and store.
 *
 * Matches x86 semantics: returns 0 on success, -1 if the walk hits an
 * unmapped intermediate or an L3 slot that was never populated. Linux
 * mprotect silently skips unmapped pages, and the caller in sys_mprotect
 * iterates page-by-page without checking our return value — so either
 * behavior is tolerated, but we match x86's -1 for consistency.
 *
 * TLB invalidation: ARM64 requires an explicit DSB + TLBI + DSB + ISB
 * sequence; arch_vmm_invlpg() handles that. */
int
vmm_set_user_prot(uint64_t pml4_phys, uint64_t virt, uint64_t flags)
{
    uint64_t l0_idx = (virt >> 39) & 0x1FF;
    uint64_t l1_idx = (virt >> 30) & 0x1FF;
    uint64_t l2_idx = (virt >> 21) & 0x1FF;
    uint64_t l3_idx = (virt >> 12) & 0x1FF;

    uint64_t *l0 = (uint64_t *)(uintptr_t)(pml4_phys + KERN_VA_OFFSET);
    uint64_t e = l0[l0_idx];
    if (!(e & PTE_VALID)) return -1;

    uint64_t *l1 = (uint64_t *)(uintptr_t)(PTE_ADDR(e) + KERN_VA_OFFSET);
    e = l1[l1_idx];
    if (!(e & PTE_VALID) || !(e & PTE_TABLE)) return -1;

    uint64_t *l2 = (uint64_t *)(uintptr_t)(PTE_ADDR(e) + KERN_VA_OFFSET);
    e = l2[l2_idx];
    if (!(e & PTE_VALID) || !(e & PTE_TABLE)) return -1;

    uint64_t *l3 = (uint64_t *)(uintptr_t)(PTE_ADDR(e) + KERN_VA_OFFSET);
    uint64_t old = l3[l3_idx];

    /* PROT_NONE on a never-mapped page is a no-op (matches x86). */
    if (!(old & PTE_VALID) && old == 0 && flags == 0)
        return 0;

    /* Any non-PROT_NONE prot change on a never-populated slot is an
     * error; the caller has no physical frame to re-permission. */
    if (old == 0 && flags != 0)
        return -1;

    uint64_t phys = PTE_ADDR(old);

    if (flags == 0) {
        /* PROT_NONE: drop VALID but keep the output address in the high
         * bits so vmm_phys_of_user_raw / sys_munmap can still find and
         * free the backing frame. */
        l3[l3_idx] = phys;
    } else {
        /* Rebuild the L3 descriptor from abstract flags. flags_to_pte()
         * forces PRESENT via caller-side OR; mirror vmm_map_user_page. */
        l3[l3_idx] = phys | flags_to_pte(flags | VMM_FLAG_PRESENT);
    }

    /* Publish the new descriptor to the MMU before invalidating the TLB. */
    __asm__ volatile("dsb ishst" ::: "memory");
    arch_vmm_invlpg(virt);
    return 0;
}

void vmm_free_user_pages(uint64_t pml4_phys) { (void)pml4_phys; }
int vmm_write_user_bytes(uint64_t pml4_phys, uint64_t va,
                         const void *src, uint64_t len) {
    const uint8_t *s = (const uint8_t *)src;
    while (len > 0) {
        uint64_t page_off = va & 0xFFF;
        uint64_t chunk = 4096 - page_off;
        if (chunk > len) chunk = len;

        uint64_t phys = vmm_phys_of_user(pml4_phys, va);
        if (!phys) return -1;

        /* Write via TTBR1 high VA. phys already includes page offset
         * (vmm_phys_of_user returns full PA, not page-aligned). */
        __builtin_memcpy((uint8_t *)(uintptr_t)(phys + KERN_VA_OFFSET), s, chunk);

        va  += chunk;
        s   += chunk;
        len -= chunk;
    }
    return 0;
}
int vmm_write_user_u64(uint64_t pml4_phys, uint64_t va, uint64_t val) {
    return vmm_write_user_bytes(pml4_phys, va, &val, 8);
}
