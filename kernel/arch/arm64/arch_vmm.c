/*
 * arch_vmm.c — ARM64 virtual memory arch primitives.
 *
 * Provides arch_vmm_load_pml4 (TTBR0_EL1) and arch_vmm_invlpg (TLBI)
 * for the shared kernel/mm/vmm.c.
 */

#include <stdint.h>

void
arch_vmm_load_pml4(uint64_t phys)
{
    __asm__ volatile(
        "msr ttbr0_el1, %0\n"
        "dsb sy\n"
        "isb\n"
        "tlbi vmalle1\n"
        "dsb sy\n"
        "isb\n"
        : : "r"(phys) : "memory"
    );
}

void
arch_vmm_invlpg(uint64_t virt)
{
    /* TLBI VAE1 — invalidate by VA, EL1.
     * The VA argument is bits [43:0] of (VA >> 12). */
    uint64_t page = virt >> 12;
    __asm__ volatile(
        "tlbi vae1, %0\n"
        "dsb sy\n"
        "isb\n"
        : : "r"(page) : "memory"
    );
}
