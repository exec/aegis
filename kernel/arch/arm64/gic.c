/*
 * gic.c — Generic Interrupt Controller dispatcher (GICv2 + GICv3).
 *
 * The historical GICv2 driver lives in this file as gic_v2_*; the
 * GICv3 driver lives in gic_v3.c. Runtime selection happens via
 * gic_set_version(), which is called from arch_mm.c after parsing the
 * DTB /intc node's `compatible` string.
 *
 * Default (if nothing called gic_set_version before gic_init): QEMU
 * virt GICv2 addresses. This keeps the kernel bootable even on the
 * ELF path where a DTB may not be provided by QEMU's -kernel shim.
 *
 * Address ranges (all accessed via the TTBR1 device-memory 1GB block
 * mapping at VA 0xFFFF000000000000 + PA):
 *
 *   QEMU virt GICv2: dist 0x08000000, cpu 0x08010000
 *   QEMU virt GICv3: dist 0x08000000, redist 0x080A0000
 *   Raspberry Pi 4 GICv2: dist 0xFF841000, cpu 0xFF842000
 *   Raspberry Pi 5 GICv2 (GIC-400, NOT v3 despite Cortex-A76):
 *     dist 0x10_7FFF_9000, cpu 0x10_7FFF_A000
 *
 * Pi 5 lives above 4 GiB, so reaching it requires the block-4 device
 * mapping that boot.S populates (kern_l1[4] → 0x100000000). All addresses
 * reach this file as PAs via gic_set_version() from the DTB walker
 * in arch_mm.c; the walker does the per-node ranges translation so
 * Pi 5's SoC-local 0x7FFF_9000 ends up as the absolute PA here.
 */

#include "arch.h"
#include "printk.h"
#include "random.h"
#include <stdint.h>

#define KERN_VA_BASE 0xFFFF000000000000UL

/* ---------------------------------------------------------------------
 * GICv2 driver (legacy — used by QEMU virt default + Pi 4).
 * --------------------------------------------------------------------- */

/* GICv2 MMIO via TTBR1 kernel mapping. Bases come from gic_set_version()
 * or the default below. Stored as uint64_t VAs so we can index them
 * with plain byte offsets. */
static uint64_t s_v2_dist_base = KERN_VA_BASE + 0x08000000UL;
static uint64_t s_v2_cpu_base  = KERN_VA_BASE + 0x08010000UL;

/* GICv2 distributor register offsets */
#define V2_GICD_CTLR        0x000
#define V2_GICD_ISENABLER   0x100
#define V2_GICD_ICENABLER   0x180
#define V2_GICD_IPRIORITYR  0x400
#define V2_GICD_ITARGETSR   0x800
#define V2_GICD_ICFGR       0xC00

/* GICv2 CPU interface register offsets */
#define V2_GICC_CTLR        0x000
#define V2_GICC_PMR         0x004
#define V2_GICC_IAR         0x00C
#define V2_GICC_EOIR        0x010

static inline uint32_t v2_r32(uint64_t base, uint32_t off) {
    return *(volatile uint32_t *)(uintptr_t)(base + off);
}
static inline void v2_w32(uint64_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(uintptr_t)(base + off) = val;
}

static void
gic_v2_init(uint64_t dist_base, uint64_t cpu_base)
{
    s_v2_dist_base = dist_base;
    s_v2_cpu_base  = cpu_base;

    /* Disable distributor during setup */
    v2_w32(s_v2_dist_base, V2_GICD_CTLR, 0);

    /* Set all SPI priorities to 0xA0 (middle priority) */
    for (int i = 8; i < 64; i++)
        v2_w32(s_v2_dist_base, V2_GICD_IPRIORITYR + 4 * i, 0xA0A0A0A0);

    /* Target all SPIs to CPU 0 */
    for (int i = 8; i < 64; i++)
        v2_w32(s_v2_dist_base, V2_GICD_ITARGETSR + 4 * i, 0x01010101);

    /* Enable distributor (group 0) */
    v2_w32(s_v2_dist_base, V2_GICD_CTLR, 1);

    /* CPU interface: enable, set priority mask to allow all */
    v2_w32(s_v2_cpu_base, V2_GICC_PMR, 0xFF);
    v2_w32(s_v2_cpu_base, V2_GICC_CTLR, 1);
}

static void
gic_v2_enable_irq(uint32_t irq)
{
    v2_w32(s_v2_dist_base, V2_GICD_ISENABLER + 4 * (irq / 32),
           1U << (irq % 32));
}

static uint32_t
gic_v2_ack_irq(void)
{
    return v2_r32(s_v2_cpu_base, V2_GICC_IAR) & 0x3FF;
}

static void
gic_v2_eoi(uint32_t irq)
{
    v2_w32(s_v2_cpu_base, V2_GICC_EOIR, irq);
}

/* ---------------------------------------------------------------------
 * GICv3 driver (gic_v3.c) — forward declarations.
 * --------------------------------------------------------------------- */

void     gic_v3_init(uint64_t dist_base, uint64_t redist_base);
void     gic_v3_enable_irq(uint32_t irq);
uint32_t gic_v3_ack_irq(void);
void     gic_v3_eoi(uint32_t irq);
void     gic_v3_timer_init(void);

/* ---------------------------------------------------------------------
 * Dispatcher — runtime selection between v2 and v3.
 * --------------------------------------------------------------------- */

#define GIC_UNKNOWN 0
#define GIC_V2      2
#define GIC_V3      3

static int      s_gic_version = GIC_UNKNOWN;
static uint64_t s_dist_base   = 0;      /* PA, not VA — we add KERN_VA_BASE below */
static uint64_t s_redist_base = 0;      /* PA — for v2 this is the CPU interface */

/* Called from arch_mm.c after DTB parse. base addresses are physical. */
void
gic_set_version(int version, uint64_t dist_pa, uint64_t redist_or_cpu_pa)
{
    s_gic_version = version;
    s_dist_base   = dist_pa;
    s_redist_base = redist_or_cpu_pa;
}

void
gic_init(void)
{
    /* If the DTB parser didn't set anything (e.g. -kernel ELF boot path
     * where QEMU does not pass a DTB in x0), use the CPU's own feature
     * register to tell v2 from v3+. ID_AA64PFR0_EL1.GIC is bits [27:24]:
     *   0 = GIC system register interface NOT implemented (→ GICv2 MMIO)
     *   1 = GICv3 or GICv4 system registers implemented
     * This is a CPU-side read so it can't fault the way an MMIO probe
     * would on an unmapped register. */
    if (s_gic_version == GIC_UNKNOWN) {
        uint64_t pfr0;
        __asm__ volatile("mrs %0, id_aa64pfr0_el1" : "=r"(pfr0));
        uint32_t gic_field = (uint32_t)((pfr0 >> 24) & 0xF);
        if (gic_field != 0) {
            s_gic_version = GIC_V3;
            s_dist_base   = 0x08000000UL;
            s_redist_base = 0x080A0000UL;   /* QEMU virt gic-version=3 */
        } else {
            s_gic_version = GIC_V2;
            s_dist_base   = 0x08000000UL;
            s_redist_base = 0x08010000UL;   /* GICC on QEMU virt GICv2 */
        }
    }

    uint64_t dist_va   = KERN_VA_BASE + s_dist_base;
    uint64_t second_va = KERN_VA_BASE + s_redist_base;

    if (s_gic_version == GIC_V3) {
        gic_v3_init(dist_va, second_va);
        printk("[GIC] OK: GICv3 initialized\n");
    } else {
        gic_v2_init(dist_va, second_va);
        printk("[GIC] OK: GICv2 initialized\n");
    }
}

void
gic_enable_irq(uint32_t irq)
{
    if (s_gic_version == GIC_V3)
        gic_v3_enable_irq(irq);
    else
        gic_v2_enable_irq(irq);
}

uint32_t
gic_ack_irq(void)
{
    return (s_gic_version == GIC_V3) ? gic_v3_ack_irq() : gic_v2_ack_irq();
}

void
gic_eoi(uint32_t irq)
{
    if (s_gic_version == GIC_V3)
        gic_v3_eoi(irq);
    else
        gic_v2_eoi(irq);
}

/* ── ARM Generic Timer ─────────────────────────────────────────────── */

/* PPI 30 = ARM generic timer (CNTP, physical, non-secure EL1) */
#define TIMER_IRQ   30

/* Timer frequency (CNTFRQ_EL0) — QEMU virt uses 62.5 MHz */
static uint64_t s_timer_freq;
static uint64_t s_ticks;

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

    /* Enable PPI 30 (timer IRQ) in the GIC. On GICv3 we go through the
     * v3 PPI enable path (redistributor SGI frame); on GICv2 it's the
     * distributor ISENABLER. The dispatcher handles both. */
    gic_enable_irq(TIMER_IRQ);

    /* Set timer to fire at 100 Hz */
    timer_set_next(s_timer_freq / 100);

    printk("[TIMER] OK: ARM generic timer at 100 Hz\n");
}

/* From sched.c */
void sched_tick(void);

void
timer_handler(void)
{
    s_ticks++;
    random_add_interrupt_entropy();
    /* Re-arm timer for next tick */
    timer_set_next(s_timer_freq / 100);
    /* Drive preemptive scheduling */
    sched_tick();
}

uint64_t
arch_get_ticks(void)
{
    return s_ticks;
}
