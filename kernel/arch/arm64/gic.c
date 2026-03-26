/*
 * gic.c — GICv2 (Generic Interrupt Controller) driver for QEMU virt.
 *
 * QEMU virt places GICv2 at:
 *   Distributor (GICD): 0x08000000
 *   CPU interface (GICC): 0x08010000
 *
 * GICv2 routes hardware interrupts (SPIs, PPIs) to CPU cores.
 * We configure it for single-core operation with a flat priority scheme.
 *
 * ARM generic timer fires PPI 30 (CNTP, physical timer, non-secure EL1).
 * This is the ARM64 equivalent of the x86 PIT.
 */

#include "arch.h"
#include "printk.h"
#include <stdint.h>

/* GICv2 MMIO base addresses (QEMU virt) */
#define GICD_BASE   0x08000000UL
#define GICC_BASE   0x08010000UL

/* Distributor registers */
#define GICD_CTLR       (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_ISENABLER(n) (*(volatile uint32_t *)(GICD_BASE + 0x100 + 4*(n)))
#define GICD_ICENABLER(n) (*(volatile uint32_t *)(GICD_BASE + 0x180 + 4*(n)))
#define GICD_IPRIORITYR(n) (*(volatile uint32_t *)(GICD_BASE + 0x400 + 4*(n)))
#define GICD_ITARGETSR(n)  (*(volatile uint32_t *)(GICD_BASE + 0x800 + 4*(n)))
#define GICD_ICFGR(n)      (*(volatile uint32_t *)(GICD_BASE + 0xC00 + 4*(n)))

/* CPU interface registers */
#define GICC_CTLR   (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR    (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_IAR    (*(volatile uint32_t *)(GICC_BASE + 0x00C))
#define GICC_EOIR   (*(volatile uint32_t *)(GICC_BASE + 0x010))

/* PPI 30 = ARM generic timer (CNTP, physical, non-secure EL1) */
#define TIMER_IRQ   30

static uint64_t s_ticks;

void
gic_init(void)
{
    /* Disable distributor during setup */
    GICD_CTLR = 0;

    /* Set all SPI priorities to 0xA0 (middle priority) */
    for (int i = 8; i < 64; i++)
        GICD_IPRIORITYR(i) = 0xA0A0A0A0;

    /* Target all SPIs to CPU 0 */
    for (int i = 8; i < 64; i++)
        GICD_ITARGETSR(i) = 0x01010101;

    /* Enable distributor (group 0) */
    GICD_CTLR = 1;

    /* CPU interface: enable, set priority mask to allow all */
    GICC_PMR  = 0xFF;
    GICC_CTLR = 1;

    printk("[GIC] OK: GICv2 initialized\n");
}

void
gic_enable_irq(uint32_t irq)
{
    GICD_ISENABLER(irq / 32) = (1U << (irq % 32));
}

uint32_t
gic_ack_irq(void)
{
    return GICC_IAR & 0x3FF;  /* interrupt ID (bits [9:0]) */
}

void
gic_eoi(uint32_t irq)
{
    GICC_EOIR = irq;
}

/* ── ARM Generic Timer ─────────────────────────────────────────────── */

/* Timer frequency (CNTFRQ_EL0) — QEMU virt uses 62.5 MHz (0x3B9ACA0) */
static uint64_t s_timer_freq;

static void
timer_set_next(uint64_t ticks_from_now)
{
    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(ticks_from_now));
    /* Enable timer, unmask */
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(1UL));
}

void
timer_init(void)
{
    /* Read timer frequency */
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(s_timer_freq));

    s_ticks = 0;

    /* Enable PPI 30 (timer IRQ) in the GIC */
    gic_enable_irq(TIMER_IRQ);

    /* Set timer to fire at 100 Hz */
    timer_set_next(s_timer_freq / 100);

    printk("[TIMER] OK: ARM generic timer at 100 Hz\n");
}

void
timer_handler(void)
{
    s_ticks++;
    /* Re-arm timer for next tick */
    timer_set_next(s_timer_freq / 100);
}

uint64_t
arch_get_ticks(void)
{
    return s_ticks;
}
