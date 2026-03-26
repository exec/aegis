#ifndef ARCH_VMM_H
#define ARCH_VMM_H

#include <stdint.h>

/*
 * ARM64 page table entry translation.
 *
 * ARM64 4KB granule PTE format (L3 page descriptor):
 *   [63]    — XN (execute never)
 *   [53]    — PXN (privileged execute never)
 *   [47:12] — output address
 *   [10]    — AF (access flag, must be 1)
 *   [9:8]   — SH (shareability: 11=inner shareable)
 *   [7:6]   — AP (access permissions: 00=EL1 RW, 01=RW all, 10=EL1 RO, 11=RO all)
 *   [4:2]   — AttrIndx (MAIR index: 0=Normal WB, 1=Device)
 *   [1]     — must be 1 for page descriptor
 *   [0]     — valid
 */

#define ARM64_PTE_VALID     (1UL << 0)
#define ARM64_PTE_PAGE      (1UL << 1)   /* L3: page descriptor (not block) */
#define ARM64_PTE_TABLE     (1UL << 1)   /* L0-L2: table descriptor */
#define ARM64_PTE_AF        (1UL << 10)
#define ARM64_PTE_SH_INNER  (3UL << 8)
#define ARM64_PTE_AP_RW_EL1 (0UL << 6)   /* EL1 RW, EL0 no access */
#define ARM64_PTE_AP_RW_ALL (1UL << 6)   /* EL1 RW, EL0 RW */
#define ARM64_PTE_XN        (1UL << 54)
#define ARM64_PTE_ATTR_NORM (0UL << 2)   /* MAIR index 0: Normal WB */
#define ARM64_PTE_ATTR_DEV  (1UL << 2)   /* MAIR index 1: Device */

/*
 * arch_pte_from_flags — translate abstract VMM_FLAG_* to ARM64 PTE bits.
 *
 * Abstract flags (from vmm.h):
 *   VMM_FLAG_PRESENT  (1<<0)  → ARM64_PTE_VALID | ARM64_PTE_PAGE | AF | SH | Normal
 *   VMM_FLAG_WRITABLE (1<<1)  → (default is RW; read-only would clear AP bit)
 *   VMM_FLAG_USER     (1<<2)  → AP_RW_ALL (EL0 accessible)
 *   VMM_FLAG_NX       (1<<63) → ARM64_PTE_XN
 *   VMM_FLAG_WC       (1<<3)  → AttrIndx=1 (Device, non-cacheable)
 *   VMM_FLAG_UCMINUS  (1<<4)  → AttrIndx=1 (Device)
 */
static inline uint64_t
arch_pte_from_flags(uint64_t flags)
{
    uint64_t pte = 0;

    if (flags & (1UL << 0))   /* PRESENT */
        pte |= ARM64_PTE_VALID | ARM64_PTE_PAGE | ARM64_PTE_AF |
               ARM64_PTE_SH_INNER | ARM64_PTE_ATTR_NORM;

    if (flags & (1UL << 2))   /* USER */
        pte |= ARM64_PTE_AP_RW_ALL;

    if (flags & (1UL << 63))  /* NX */
        pte |= ARM64_PTE_XN;

    /* WC or UCMINUS → device memory (non-cacheable) */
    if (flags & ((1UL << 3) | (1UL << 4))) {
        pte &= ~(7UL << 2);           /* clear AttrIndx */
        pte |= ARM64_PTE_ATTR_DEV;    /* AttrIndx = 1 */
        pte &= ~ARM64_PTE_SH_INNER;   /* device memory: non-shareable */
    }

    return pte;
}

/* Extract physical address from an ARM64 PTE (bits [47:12]). */
#define ARCH_PTE_ADDR(e) ((e) & 0x0000FFFFFFFFF000UL)

#endif /* ARCH_VMM_H */
