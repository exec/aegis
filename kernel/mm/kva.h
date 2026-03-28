#ifndef AEGIS_KVA_H
#define AEGIS_KVA_H

#include <stdint.h>

/* kva_init — initialise the kernel virtual allocator.
 * Must be called after vmm_init() (requires the mapped-window allocator).
 * Prints [KVA] OK on success. */
void kva_init(void);

/* kva_alloc_pages — allocate n 4KB pages, map them to consecutive higher-half
 * virtual addresses, and return the base VA as a pointer.
 * Panics on PMM exhaustion. Never pass VMM_FLAG_USER — kva pages are kernel-only;
 * USER must be absent so the MMU denies ring-3 access to kernel objects. */
void *kva_alloc_pages(uint64_t n);

/* kva_map_phys_pages — map num_pages of existing physical memory starting at
 * phys_base into contiguous kernel VA. Does NOT allocate physical pages from
 * PMM — the pages must already exist (e.g. GRUB-loaded module).
 * phys_base must be 4KB-aligned. Returns the virtual base address. */
void *kva_map_phys_pages(uint64_t phys_base, uint32_t num_pages);

/* kva_page_phys — return the physical address of the page mapped at va.
 * va must be a VA previously returned by kva_alloc_pages (or offset within
 * such a range). Panics if any page-table level is absent. */
uint64_t kva_page_phys(void *va);

/* kva_free_pages — return n 4KB pages at va to the PMM.
 *
 * For each page i in [0, n): recover phys via vmm_phys_of, unmap the VA
 * via vmm_unmap_page, then return the frame via pmm_free_page. The virtual
 * address range is permanently abandoned — the bump cursor is not rewound;
 * VA space is not the scarce resource.
 *
 * va must be the base of a contiguous kva allocation. Panics if any page
 * is not mapped (calling with an unallocated VA is a kernel bug). */
void kva_free_pages(void *va, uint64_t n);

#endif /* AEGIS_KVA_H */
