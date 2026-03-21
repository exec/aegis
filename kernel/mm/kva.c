#include "kva.h"
#include "vmm.h"
#include "pmm.h"
#include "printk.h"
#include "arch.h"
#include <stdint.h>

/* KVA_BASE: pd_hi[4] range — 0x800000 bytes above ARCH_KERNEL_VIRT_BASE.
 * pd_hi[0..1] = kernel image huge pages
 * pd_hi[2]    = (formerly KSTACK_VA — now also kva range)
 * pd_hi[3]    = VMM_WINDOW_VA (Phase 6)
 * pd_hi[4+]   = kva bump range (this allocator) */
#define KVA_BASE (ARCH_KERNEL_VIRT_BASE + 0x800000UL)

static uint64_t s_kva_next;

void
kva_init(void)
{
    s_kva_next = KVA_BASE;
    printk("[KVA] OK: kernel virtual allocator active\n");
}

void *
kva_alloc_pages(uint64_t n)
{
    uint64_t base = s_kva_next;
    uint64_t i;
    for (i = 0; i < n; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            printk("[KVA] FAIL: out of memory\n");
            for (;;) {}
        }
        /* IMPORTANT: never pass VMM_FLAG_USER here. kva pages are mapped into
         * pd_hi which is shared with user PML4s (same physical pd_hi page).
         * Absent VMM_FLAG_USER, the MMU blocks user-mode access to these PTEs.
         * Setting VMM_FLAG_USER would expose all kernel objects to ring-3. */
        vmm_map_page(s_kva_next, phys, VMM_FLAG_WRITABLE);
        s_kva_next += 4096UL;
    }
    /* SAFETY: base is a higher-half VA set from s_kva_next at function entry;
     * all n pages at [base, s_kva_next) have been mapped via vmm_map_page. */
    return (void *)base;
}

uint64_t
kva_page_phys(void *va)
{
    return vmm_phys_of((uint64_t)(uintptr_t)va);
}

void
kva_free_pages(void *va, uint64_t n)
{
    uint64_t addr = (uint64_t)(uintptr_t)va;
    uint64_t i;
    for (i = 0; i < n; i++) {
        uint64_t page_va = addr + i * 4096UL;
        uint64_t phys    = vmm_phys_of(page_va);
        vmm_unmap_page(page_va);
        pmm_free_page(phys);
    }
}
