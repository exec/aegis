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
#define UART_IRQ  33   /* PL011 UART0 = SPI 1 = IRQ 33 */

/* From uart_pl011.c */
void uart_rx_handler(void);

void
irq_handler(void)
{
    uint32_t irq = gic_ack_irq();

    if (irq == TIMER_IRQ) {
        timer_handler();
    } else if (irq == UART_IRQ) {
        uart_rx_handler();
    } else if (irq < 1020) {
        printk("[IRQ] unhandled IRQ %u\n", irq);
    }

    if (irq < 1020)
        gic_eoi(irq);
}

void
exc_sync_handler(void)
{
    uint64_t esr, elr, far;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    printk("[PANIC] kernel sync at ELR=0x%lx ESR=0x%lx FAR=0x%lx\n", elr, esr, far);
    for (;;)
        __asm__ volatile("wfi");
}

/* Debug: log first few syscalls */
static int s_syscall_debug_count = 0;

/* Called from vectors.S before syscall_dispatch (for debug) */
void
syscall_debug(uint64_t num, uint64_t a1, uint64_t a2)
{
    if (s_syscall_debug_count++ < 5)
        printk("[SVC] syscall %lu arg1=0x%lx arg2=0x%lx\n", num, a1, a2);
}

/* Non-SVC synchronous exception from EL0 (e.g. data abort, undef) */
void
exc_sync_el0_handler(void *frame)
{
    uint64_t esr;
    uint64_t elr;
    (void)frame;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
    printk("[PANIC] user sync exception at ELR=0x%lx ESR=0x%lx EC=0x%x\n",
           elr, esr, (uint32_t)(esr >> 26));
    for (;;)
        __asm__ volatile("wfi");
}
