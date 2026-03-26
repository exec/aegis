/*
 * mmu_early.c — Early MMU page table setup (TTBR0 + TTBR1).
 *
 * TTBR0 (identity map, for boot transition):
 *   boot_l0[0] → boot_l1
 *   boot_l1[0] → 0x00000000 (1GB device)
 *   boot_l1[1] → 0x40000000 (1GB RAM)
 *
 * TTBR1 (kernel higher-half, 0xFFFF000000000000+):
 *   kern_l0[0] → kern_l1
 *   kern_l1[0] → 0x00000000 (1GB device — UART, GIC)
 *   kern_l1[1] → 0x40000000 (1GB RAM — kernel image)
 */

#include <stdint.h>

#define PTE_VALID       (1UL << 0)
#define PTE_TABLE       (1UL << 1)
#define PTE_AF          (1UL << 10)
#define PTE_SH_INNER    (3UL << 8)
#define PTE_AP_RW_EL1   (0UL << 6)
#define PTE_ATTR_NORMAL (0UL << 2)
#define PTE_ATTR_DEVICE (1UL << 2)

/* Page tables in BSS (linked at high VMA, accessed at PA before MMU). */
uint64_t boot_l0[512] __attribute__((aligned(4096)));
uint64_t boot_l1[512] __attribute__((aligned(4096)));
uint64_t kern_l0[512] __attribute__((aligned(4096)));
uint64_t kern_l1[512] __attribute__((aligned(4096)));

#define VA_OFF 0xFFFF000000000000UL
#define TO_PHYS(ptr) ((uint64_t)(uintptr_t)(ptr) - VA_OFF)

void
mmu_early_init(void)
{
    int i;
    uint64_t *bl0 = (uint64_t *)TO_PHYS(boot_l0);
    uint64_t *bl1 = (uint64_t *)TO_PHYS(boot_l1);
    uint64_t *kl0 = (uint64_t *)TO_PHYS(kern_l0);
    uint64_t *kl1 = (uint64_t *)TO_PHYS(kern_l1);

    for (i = 0; i < 512; i++) {
        bl0[i] = 0; bl1[i] = 0;
        kl0[i] = 0; kl1[i] = 0;
    }

    /* TTBR0 identity map */
    bl0[0] = TO_PHYS(boot_l1) | PTE_VALID | PTE_TABLE;
    bl1[0] = 0x00000000UL | PTE_VALID | PTE_AF | PTE_ATTR_DEVICE | PTE_AP_RW_EL1;
    bl1[1] = 0x40000000UL | PTE_VALID | PTE_AF | PTE_SH_INNER | PTE_ATTR_NORMAL | PTE_AP_RW_EL1;

    /* TTBR1 kernel map (0xFFFF0000_00000000+) */
    kl0[0] = TO_PHYS(kern_l1) | PTE_VALID | PTE_TABLE;
    kl1[0] = 0x00000000UL | PTE_VALID | PTE_AF | PTE_ATTR_DEVICE | PTE_AP_RW_EL1;
    kl1[1] = 0x40000000UL | PTE_VALID | PTE_AF | PTE_SH_INNER | PTE_ATTR_NORMAL | PTE_AP_RW_EL1;
}
