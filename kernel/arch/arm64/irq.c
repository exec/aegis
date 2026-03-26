/*
 * irq.c — ARM64 IRQ dispatch.
 *
 * Called from _exc_irq_el1 in vectors.S after saving registers.
 * Reads the GIC IAR to get the interrupt ID, dispatches to the
 * appropriate handler, then signals EOI.
 */

#include "printk.h"
#include <stdint.h>

/* From gic.c */
uint32_t gic_ack_irq(void);
void     gic_eoi(uint32_t irq);
void     timer_handler(void);

#define TIMER_IRQ 30

void
irq_handler(void)
{
    uint32_t irq = gic_ack_irq();

    if (irq == TIMER_IRQ) {
        timer_handler();
    } else if (irq < 1020) {
        /* Spurious or unhandled IRQ */
        printk("[IRQ] unhandled IRQ %u\n", irq);
    }
    /* IRQ 1023 = spurious, no EOI needed */

    if (irq < 1020)
        gic_eoi(irq);
}

void
exc_sync_handler(void)
{
    uint64_t esr;
    uint64_t elr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
    printk("[PANIC] synchronous exception at ELR=0x%lx ESR=0x%lx\n", elr, esr);
    for (;;)
        __asm__ volatile("wfi");
}
