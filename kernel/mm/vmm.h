#ifndef AEGIS_VMM_H
#define AEGIS_VMM_H

#include <stdint.h>

/* Page mapping flags. These map to x86-64 PTE bits but the interface is
 * intentionally arch-agnostic — arch/x86_64/arch_vmm.c handles hardware. */
#define VMM_FLAG_PRESENT  (1UL << 0)
#define VMM_FLAG_WRITABLE (1UL << 1)
#define VMM_FLAG_USER     (1UL << 2)
#define VMM_FLAG_NX       (1UL << 63)

/* vmm_init — build the initial higher-half page tables and activate them.
 * Must be called after pmm_init(). Prints [VMM] OK on success. */
void vmm_init(void);

/* vmm_map_page — map a single 4KB page.
 * virt and phys must be 4KB-aligned. flags is a combination of VMM_FLAG_*. */
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/* vmm_unmap_page — unmap a single 4KB page and invalidate its TLB entry.
 * virt must be 4KB-aligned and must currently be mapped. */
void vmm_unmap_page(uint64_t virt);

#endif /* AEGIS_VMM_H */
