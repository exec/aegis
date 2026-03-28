#include "kva.h"
#include "vmm.h"
#include "pmm.h"
#include "printk.h"
#include "arch.h"
#include "spinlock.h"
#include <stdint.h>
#include <stddef.h>

/* KVA_BASE: start of the bump-allocated kernel VA range.
 * Each architecture defines ARCH_KVA_BASE in arch.h to place this past
 * the kernel image, window allocator, and any other fixed-VA regions.
 * x86-64: pd_hi[4] = VIRT_BASE + 0x800000
 * ARM64:  L2[5]    = VIRT_BASE + 0xA00000 */
#ifdef ARCH_KVA_BASE
#define KVA_BASE ARCH_KVA_BASE
#else
#define KVA_BASE (ARCH_KERNEL_VIRT_BASE + 0x800000UL)
#endif

static uint64_t s_kva_next;
static spinlock_t kva_lock = SPINLOCK_INIT;

void
kva_init(void)
{
    s_kva_next = KVA_BASE;
    printk("[KVA] OK: kernel virtual allocator active\n");
}

void *
kva_alloc_pages(uint64_t n)
{
    if (n == 0) return NULL;   /* caller passed 0 — no pages to allocate */
    irqflags_t fl = spin_lock_irqsave(&kva_lock);
    uint64_t base = s_kva_next;
    uint64_t i;
    for (i = 0; i < n; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            spin_unlock_irqrestore(&kva_lock, fl);
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
    spin_unlock_irqrestore(&kva_lock, fl);
    /* SAFETY: base is a higher-half VA set from s_kva_next at function entry;
     * all n pages at [base, s_kva_next) have been mapped via vmm_map_page. */
    return (void *)base;
}

void *
kva_map_phys_pages(uint64_t phys_base, uint32_t num_pages)
{
    if (num_pages == 0) return NULL;
    uint64_t base = s_kva_next;
    uint32_t i;
    for (i = 0; i < num_pages; i++) {
        vmm_map_page(s_kva_next, phys_base + (uint64_t)i * 4096UL,
                     VMM_FLAG_WRITABLE);
        s_kva_next += 4096UL;
    }
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
