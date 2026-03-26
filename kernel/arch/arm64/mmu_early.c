/*
 * mmu_early.c — Early MMU setup for ARM64 (before kernel_main).
 *
 * Builds a minimal identity map using L1 block descriptors (1GB each):
 *   L0[0] → L1 table
 *   L1[0] → 0x00000000 (1GB device memory: UART, GIC, etc.)
 *   L1[1] → 0x40000000 (1GB normal memory: RAM)
 *
 * Called from boot.S before kernel_main. After this returns, boot.S
 * enables the MMU via SCTLR_EL1.
 *
 * ARM64 4KB granule, 48-bit VA:
 *   L0: bits [47:39] — 512 entries, each covers 512GB
 *   L1: bits [38:30] — 512 entries, each covers 1GB (block descriptors OK)
 *   L2: bits [29:21] — 512 entries, each covers 2MB
 *   L3: bits [20:12] — 512 entries, each covers 4KB
 */

#include <stdint.h>

/* Page table entry bits */
#define PTE_VALID       (1UL << 0)
#define PTE_TABLE       (1UL << 1)   /* L0/L1/L2: next level is a table */
#define PTE_BLOCK       (0UL << 1)   /* L1/L2: this is a block mapping */
#define PTE_AF          (1UL << 10)  /* Access Flag — must be set or #PF */
#define PTE_SH_INNER    (3UL << 8)   /* Inner Shareable */
#define PTE_AP_RW_EL1   (0UL << 6)   /* AP[2:1] = 00 → EL1 RW, EL0 no access */

/* MAIR attribute indices (must match MAIR_EL1 setup in boot.S) */
#define PTE_ATTR_NORMAL (0UL << 2)   /* AttrIndx = 0 → Normal WB */
#define PTE_ATTR_DEVICE (1UL << 2)   /* AttrIndx = 1 → Device-nGnRnE */

/* Boot page tables — in BSS, 4KB aligned.
 * These are physical addresses (MMU off when this code runs). */
uint64_t boot_l0[512] __attribute__((aligned(4096)));
uint64_t boot_l1[512] __attribute__((aligned(4096)));

void
mmu_early_init(void)
{
    /* Zero tables (BSS should be zeroed, but be safe) */
    for (int i = 0; i < 512; i++) {
        boot_l0[i] = 0;
        boot_l1[i] = 0;
    }

    /* L0[0] → L1 table */
    boot_l0[0] = (uint64_t)(uintptr_t)boot_l1 | PTE_VALID | PTE_TABLE;

    /* L1[0] → 0x00000000 (1GB block, device memory)
     * Covers PL011 UART (0x09000000), GIC (0x08000000), etc. */
    boot_l1[0] = 0x00000000UL | PTE_VALID | PTE_BLOCK | PTE_AF |
                 PTE_ATTR_DEVICE | PTE_AP_RW_EL1;

    /* L1[1] → 0x40000000 (1GB block, normal cacheable memory)
     * Covers RAM where the kernel and data live. */
    boot_l1[1] = 0x40000000UL | PTE_VALID | PTE_BLOCK | PTE_AF |
                 PTE_ATTR_NORMAL | PTE_SH_INNER | PTE_AP_RW_EL1;

    /* boot_l0 physical address is returned to boot.S via x0.
     * boot.S writes it to TTBR0_EL1 and enables the MMU. */
}
