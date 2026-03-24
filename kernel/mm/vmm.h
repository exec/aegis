#ifndef AEGIS_VMM_H
#define AEGIS_VMM_H

#include <stdint.h>

/* Page mapping flags. These are x86-64 PTE bit positions and are passed
 * directly into page table entries by vmm.c. An ARM64 port would need
 * arch_vmm.c to translate these before inserting into hardware PTEs. */
#define VMM_FLAG_PRESENT  (1UL << 0)
#define VMM_FLAG_WRITABLE (1UL << 1)
#define VMM_FLAG_USER     (1UL << 2)
/* Cache type bits — only valid after arch_pat_init() has run.
 *
 * With IA32_PAT[1]=WC (set by arch_pat_init):
 *   VMM_FLAG_WC      = PWT bit → PAT entry 1 → Write-Combining
 *   VMM_FLAG_UCMINUS = PCD bit → PAT entry 2 → UC- (weak uncacheable)
 *
 * NOTE: UC- (PA2) is NOT the same as strong-UC (PA3 = PWT|PCD = 0x18, used
 * for PCIe ECAM). Use VMM_FLAG_UCMINUS only where MTRR override is acceptable.
 *
 * These map to x86 PTE bits 3 (PWT) and 4 (PCD).
 * vmm_map_page ORs flags into the PTE without masking, so these pass
 * through as-is to hardware. */
#define VMM_FLAG_WC       (1UL << 3)   /* Write-Combining  (PWT=1, PCD=0) → PA1 */
#define VMM_FLAG_UCMINUS  (1UL << 4)   /* UC-weak          (PWT=0, PCD=1) → PA2 */
#define VMM_FLAG_NX       (1UL << 63)

/* vmm_init — build the initial higher-half page tables and activate them.
 * Must be called after pmm_init(). Prints [VMM] OK on success. */
void vmm_init(void);

/* vmm_map_page — map a single 4KB page.
 * virt and phys must be 4KB-aligned. flags is a combination of VMM_FLAG_*. */
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/* vmm_unmap_page — unmap a single 4KB page and invalidate its TLB entry.
 * virt must be 4KB-aligned and must currently be mapped.
 * Valid for 4KB mappings only. Must not be called on addresses backed by
 * 2MB huge pages; doing so will panic the kernel. */
void vmm_unmap_page(uint64_t virt);

/* Allocate a new PML4 and copy kernel high entries [256..511] from the
 * master PML4. Returns physical address of the new PML4.
 * Valid while identity map [0..4MB) is active (Phase 5 constraint). */
uint64_t vmm_create_user_pml4(void);

/* Map a single 4KB page in pml4_phys (NOT the active kernel PML4).
 * All intermediate page-table entries (PML4e, PDPTe, PDe) created for
 * this mapping have VMM_FLAG_USER set — required because the x86-64 MMU
 * checks the USER bit at every level of the page-table walk.
 * A leaf PTE with USER set but any ancestor without USER causes a ring-3
 * #PF even if the leaf mapping is correct.
 * flags: VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE as needed. */
void vmm_map_user_page(uint64_t pml4_phys, uint64_t virt,
                       uint64_t phys, uint64_t flags);

/* Load pml4_phys into CR3. Flushes TLB. */
void vmm_switch_to(uint64_t pml4_phys);

/* Return the physical address of the master (kernel) PML4.
 * Used by the scheduler to restore the master PML4 after a user task runs. */
uint64_t vmm_get_master_pml4(void);

/* vmm_phys_of — return the physical address of the 4KB page mapped at virt.
 * Uses the walk-overwrite window pattern. Panics if any level is not present. */
uint64_t vmm_phys_of(uint64_t virt);

/* vmm_teardown_identity — clear pml4[0] (the [0..512GB) identity range)
 * and reload CR3 for a full TLB flush. Must be called after all kernel
 * objects have been allocated via kva (so no identity-map cast remains).
 * Prints [VMM] OK: identity map removed. */
void vmm_teardown_identity(void);

/* vmm_free_user_pml4 — walk PML4 entries 0–255 (user half) and free:
 *   - all leaf physical frames (PT entries)
 *   - all intermediate page-table pages (PT, PD, PDPT)
 *   - the PML4 page itself
 *
 * MUST NOT touch PML4 entries 256–511 (kernel half): those pages are shared
 * with the master PML4; freeing them corrupts every other process and the
 * kernel itself.
 *
 * Uses the mapped-window allocator for all page-table accesses.
 * Caller must have switched to the master PML4 before calling (sched_exit
 * does this at entry). Single-CPU only: no TLB shootdown. */
void vmm_free_user_pml4(uint64_t pml4_phys);

/* vmm_phys_of_user — walk pml4_phys to find the physical address mapped at virt.
 * Returns physical address, or 0 if not mapped.
 * Uses the window allocator. Safe to call with any PML4 (not just active CR3). */
uint64_t vmm_phys_of_user(uint64_t pml4_phys, uint64_t virt);

/* vmm_unmap_user_page — clear the PTE for virt in pml4_phys and invlpg.
 * Does not free the physical page. Caller frees via pmm_free_page.
 * Silent no-op if the page is not mapped. */
void vmm_unmap_user_page(uint64_t pml4_phys, uint64_t virt);

/* vmm_zero_page — zero the physical page at phys using the mapped-window slot.
 * Required for MAP_ANONYMOUS: musl's heap allocator depends on zeroed pages.
 * Uses the internal vmm_window_map/vmm_window_unmap pair. */
void vmm_zero_page(uint64_t phys);

/* vmm_copy_user_pages — full copy of all user-half (PML4 entries 0-255) pages
 * from src_pml4 to dst_pml4.
 * For each present user leaf PTE in src_pml4: allocates a new frame via pmm,
 * copies contents via the two-window-slot mechanism, maps into dst_pml4.
 * Returns 0 on success, -1 on OOM.
 * Called by sys_fork to create the child address space. */
int vmm_copy_user_pages(uint64_t src_pml4, uint64_t dst_pml4);

/* vmm_free_user_pages — free all leaf physical frames mapped in user half
 * (PML4 entries 0-255) of pml4_phys. Does NOT free PT/PD/PDPT pages and
 * does NOT free the PML4 itself. Zeros the leaf PTEs after freeing.
 *
 * Contrast with vmm_free_user_pml4 which also frees the page-table pages.
 * Use vmm_free_user_pages for sys_execve (process keeps its PML4 and
 * reloads new mappings). Use vmm_free_user_pml4 for waitpid zombie reap.
 *
 * Caller must reload CR3 after calling to flush stale TLB entries. */
void vmm_free_user_pages(uint64_t pml4_phys);

/* vmm_write_user_bytes — copy len bytes from kernel src into user VA range
 * [va, va+len) in pml4_phys.  Handles writes that cross page boundaries.
 * Uses the mapped-window allocator; caller must ensure pages are mapped.
 * Returns 0 on success, -1 if a page is not mapped. */
int vmm_write_user_bytes(uint64_t pml4_phys, uint64_t va,
                         const void *src, uint64_t len);

/* vmm_write_user_u64 — write one uint64_t value to user VA in pml4_phys.
 * Returns 0 on success, -1 if the page is not mapped.
 * Convenience wrapper around vmm_write_user_bytes. */
int vmm_write_user_u64(uint64_t pml4_phys, uint64_t va, uint64_t val);

#endif /* AEGIS_VMM_H */
