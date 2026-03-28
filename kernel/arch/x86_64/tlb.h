/* tlb.h -- TLB shootdown for SMP page table coherence.
 *
 * When one CPU modifies user-space page tables (munmap, mprotect, execve),
 * other CPUs that may have cached TLB entries for the same address space
 * must invalidate them.  tlb_shootdown() sends an IPI to all other online
 * CPUs and waits for acknowledgement before returning.
 *
 * On single-CPU systems (or before LAPIC init), tlb_shootdown() falls back
 * to local invlpg only.
 */
#ifndef AEGIS_TLB_H
#define AEGIS_TLB_H

#include <stdint.h>

/* Invalidate TLB entries for a VA range in a specific address space.
 * Sends IPI to all other online CPUs, does local invlpg as well.
 * va_start must be page-aligned.  va_end is exclusive. */
void tlb_shootdown(uint64_t target_cr3, uint64_t va_start, uint64_t va_end);

/* ISR handler for the TLB shootdown IPI vector (0xFE).
 * Called from isr_dispatch; sends LAPIC EOI internally. */
void tlb_shootdown_handler(void);

/* Flush the entire TLB on the local CPU (reload CR3). */
void tlb_flush_local(void);

#endif /* AEGIS_TLB_H */
