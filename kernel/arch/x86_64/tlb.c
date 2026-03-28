/* tlb.c -- TLB shootdown via IPI for SMP page table coherence.
 *
 * A single shared shootdown request is protected by s_shootdown_lock.
 * The initiator fills in the target CR3 and VA range, sends a broadcast
 * IPI (vector 0xFE), and spins until all target CPUs acknowledge.
 *
 * Receivers check whether their current CR3 matches the target; if so
 * they execute invlpg for each page in the range.  If CR3 doesn't match,
 * the stale entries will be flushed on the next context switch (CR3 load
 * flushes all non-global TLB entries).
 */

#include "tlb.h"
#include "lapic.h"
#include "smp.h"
#include "arch.h"
#include "spinlock.h"
#include <stdint.h>

#define TLB_SHOOTDOWN_VECTOR 0xFE

/* Shared shootdown request (protected by s_shootdown_lock). */
static spinlock_t        s_shootdown_lock = SPINLOCK_INIT;
static volatile uint64_t s_target_cr3;
static volatile uint64_t s_va_start;
static volatile uint64_t s_va_end;
static volatile uint16_t s_pending;  /* bitmask: CPUs that must respond */
static volatile uint16_t s_ack;      /* bitmask: CPUs that have responded */

void
tlb_shootdown(uint64_t target_cr3, uint64_t va_start, uint64_t va_end)
{
    /* Single-core or pre-LAPIC: local invlpg only. */
    if (!lapic_active() || g_cpu_count <= 1) {
        for (uint64_t va = va_start; va < va_end; va += 4096)
            __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
        return;
    }

    irqflags_t fl = spin_lock_irqsave(&s_shootdown_lock);

    /* Build target bitmask: all online CPUs except ourselves.
     * We conservatively include every online CPU rather than checking
     * per-CPU CR3, because we cannot safely read another CPU's CR3. */
    uint8_t my_id = percpu_self()->cpu_id;
    uint16_t target_mask = 0;
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (i == my_id)
            continue;
        if (!g_ap_online[i])
            continue;
        target_mask |= (uint16_t)(1u << i);
    }

    /* Local invalidation first. */
    for (uint64_t va = va_start; va < va_end; va += 4096)
        __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");

    if (target_mask == 0) {
        spin_unlock_irqrestore(&s_shootdown_lock, fl);
        return;
    }

    /* Set up shared request. */
    s_target_cr3 = target_cr3;
    s_va_start   = va_start;
    s_va_end     = va_end;
    s_ack        = 0;
    __atomic_store_n(&s_pending, target_mask, __ATOMIC_RELEASE);

    /* Broadcast IPI to all other CPUs. */
    lapic_send_ipi_all_excl_self(TLB_SHOOTDOWN_VECTOR);

    /* Spin until all targets acknowledge. */
    while (__atomic_load_n(&s_ack, __ATOMIC_ACQUIRE) != target_mask)
        arch_pause();

    spin_unlock_irqrestore(&s_shootdown_lock, fl);
}

void
tlb_shootdown_handler(void)
{
    uint8_t my_id  = percpu_self()->cpu_id;
    uint16_t my_bit = (uint16_t)(1u << my_id);

    if (__atomic_load_n(&s_pending, __ATOMIC_ACQUIRE) & my_bit) {
        /* Check if our CR3 matches the target. */
        uint64_t my_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(my_cr3));

        if (my_cr3 == s_target_cr3) {
            for (uint64_t va = s_va_start; va < s_va_end; va += 4096)
                __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
        }
        /* If CR3 doesn't match, stale TLB entries will be flushed on the
         * next context switch when the new CR3 is loaded. */

        __atomic_or_fetch(&s_ack, my_bit, __ATOMIC_RELEASE);
    }

    lapic_eoi();
}

void
tlb_flush_local(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}
