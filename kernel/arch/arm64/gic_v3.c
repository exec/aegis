/*
 * gic_v3.c — GICv3 (Generic Interrupt Controller v3) driver.
 *
 * Used by:
 *   - QEMU virt with `-machine virt,gic-version=3` (distributor 0x08000000,
 *     redistributor 0x080A0000)
 *   - Raspberry Pi 5 / BCM2712 (GIC-600)
 *
 * GICv3 replaces the GICv2 CPU interface MMIO with system registers
 * (ICC_*) and introduces a per-CPU redistributor block that must be
 * woken before interrupts work. The distributor is still MMIO, but
 * register layout differs slightly (e.g. affinity routing via GICD_CTLR
 * ARE bit + GICD_IROUTER for SPIs).
 *
 * This driver only supports single-CPU operation for now: the BSP
 * wakes redistributor 0, configures SGI/PPIs there, and routes all
 * SPIs to affinity 0 via GICD_IROUTER. SMP is deferred to phase B3.
 *
 * See ARM64.md §17.2 for the design rationale and the plan.
 * Spec reference: ARM IHI 0069 (GICv3/v4 architecture).
 */

#include "printk.h"
#include <stdint.h>

/* ---------------------------------------------------------------------
 * MMIO accessors — the caller passes the kernel-VA-mapped base; we
 * index into it with byte offsets.
 * --------------------------------------------------------------------- */

static inline uint32_t mmio_r32(uint64_t base, uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(base + off);
}
static inline void mmio_w32(uint64_t base, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)(base + off) = val;
}
static inline void mmio_w64(uint64_t base, uint32_t off, uint64_t val)
{
    *(volatile uint64_t *)(uintptr_t)(base + off) = val;
}

/* ---------------------------------------------------------------------
 * GICD (distributor) register offsets — shared with GICv2 but with
 * ARE (affinity routing enable) + IROUTER for SPI routing.
 * --------------------------------------------------------------------- */

#define GICD_CTLR               0x0000
#define GICD_TYPER              0x0004
#define GICD_IIDR               0x0008
#define GICD_IGROUPR(n)         (0x0080 + 4 * (n))
#define GICD_ISENABLER(n)       (0x0100 + 4 * (n))
#define GICD_ICENABLER(n)       (0x0180 + 4 * (n))
#define GICD_ICPENDR(n)         (0x0280 + 4 * (n))
#define GICD_IPRIORITYR(n)      (0x0400 + 4 * (n))
#define GICD_ICFGR(n)           (0x0C00 + 4 * (n))
#define GICD_IROUTER(n)         (0x6000 + 8 * (n))   /* 64-bit, one per SPI */

/* GICD_CTLR (secure/non-secure banked; we run NS EL1 on QEMU virt) */
#define GICD_CTLR_EnableGrp1NS  (1U << 1)
#define GICD_CTLR_ARE_NS        (1U << 4)
#define GICD_CTLR_RWP           (1U << 31)

/* ---------------------------------------------------------------------
 * GICR (redistributor) register offsets.
 *
 * Each redistributor is 128 KiB: an "RD" frame at offset 0 and an
 * "SGI" frame at offset 0x10000. The redistributors are laid out in
 * a contiguous block; on QEMU virt at gic-version=3 the block starts
 * at 0x080A0000 and each redistributor is 128 KiB stride. For a
 * single-core boot we only touch redistributor 0.
 * --------------------------------------------------------------------- */

#define GICR_CTLR               0x0000
#define GICR_TYPER              0x0008
#define GICR_WAKER              0x0014

#define GICR_WAKER_ProcessorSleep   (1U << 1)
#define GICR_WAKER_ChildrenAsleep   (1U << 2)

/* SGI/PPI frame = redistributor base + 0x10000 */
#define GICR_SGI_OFFSET         0x10000
#define GICR_IGROUPR0           0x0080   /* within SGI frame */
#define GICR_ISENABLER0         0x0100
#define GICR_ICENABLER0         0x0180
#define GICR_ICPENDR0           0x0280
#define GICR_IPRIORITYR(n)      (0x0400 + 4 * (n))
#define GICR_ICFGR0             0x0C00   /* SGI config (read-only) */
#define GICR_ICFGR1             0x0C04   /* PPI config (edge/level) */

/* ---------------------------------------------------------------------
 * ICC_* system register accessors.
 *
 * GAS in the bundled aarch64-elf toolchain accepts the short names for
 * all ICC_* registers we need at the armv8.0 baseline (SRE/PMR/IGRPEN1
 * are always present; IAR1/EOIR1/BPR1 come with GICv3 system-register
 * support, which QEMU virt + Cortex-A76 advertise). Use the names for
 * readability — no Sn_m_Cx_Cy_z encodings needed.
 * --------------------------------------------------------------------- */

static inline uint64_t read_icc_sre_el1(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, ICC_SRE_EL1" : "=r"(v));
    return v;
}
static inline void write_icc_sre_el1(uint64_t v)
{
    __asm__ volatile("msr ICC_SRE_EL1, %0" : : "r"(v));
    __asm__ volatile("isb");
}
static inline void write_icc_pmr_el1(uint64_t v)
{
    __asm__ volatile("msr ICC_PMR_EL1, %0" : : "r"(v));
}
static inline void write_icc_bpr1_el1(uint64_t v)
{
    __asm__ volatile("msr ICC_BPR1_EL1, %0" : : "r"(v));
}
static inline void write_icc_igrpen1_el1(uint64_t v)
{
    __asm__ volatile("msr ICC_IGRPEN1_EL1, %0" : : "r"(v));
    __asm__ volatile("isb");
}
static inline uint64_t read_icc_iar1_el1(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, ICC_IAR1_EL1" : "=r"(v));
    return v;
}
static inline void write_icc_eoir1_el1(uint64_t v)
{
    __asm__ volatile("msr ICC_EOIR1_EL1, %0" : : "r"(v));
}

/* ---------------------------------------------------------------------
 * Driver state.
 * --------------------------------------------------------------------- */

static uint64_t s_dist_base;       /* GICD VA */
static uint64_t s_redist_base;     /* RD_base[0] VA */
static uint64_t s_sgi_base;        /* RD_base[0] + 0x10000 VA */
static uint32_t s_max_spi;         /* highest supported SPI (from GICD_TYPER) */

/* Spin on RWP (register write pending). */
static void
gicd_wait_rwp(void)
{
    while (mmio_r32(s_dist_base, GICD_CTLR) & GICD_CTLR_RWP)
        __asm__ volatile("yield");
}

/* Wake redistributor 0 for the BSP. */
static void
gicr_wake_bsp(void)
{
    uint32_t waker = mmio_r32(s_redist_base, GICR_WAKER);
    waker &= ~GICR_WAKER_ProcessorSleep;
    mmio_w32(s_redist_base, GICR_WAKER, waker);
    /* Wait for ChildrenAsleep to clear. */
    while (mmio_r32(s_redist_base, GICR_WAKER) & GICR_WAKER_ChildrenAsleep)
        __asm__ volatile("yield");
}

/* ---------------------------------------------------------------------
 * Public API — matches gic.c signatures so the dispatcher can route
 * identically for v2/v3.
 * --------------------------------------------------------------------- */

void
gic_v3_init(uint64_t dist_base, uint64_t redist_base)
{
    s_dist_base   = dist_base;
    s_redist_base = redist_base;
    s_sgi_base    = redist_base + GICR_SGI_OFFSET;

    /* Read GICD_TYPER.ITLinesNumber (bits [4:0]) to size the SPI loop.
     * ITLinesNumber N => supported INTIDs = 32 * (N + 1). */
    uint32_t typer = mmio_r32(s_dist_base, GICD_TYPER);
    uint32_t it_lines = (typer & 0x1F) + 1;
    s_max_spi = (it_lines * 32) - 1;
    if (s_max_spi > 1019) s_max_spi = 1019;

    /* 1. Disable the distributor before touching its registers. */
    mmio_w32(s_dist_base, GICD_CTLR, 0);
    gicd_wait_rwp();

    /* 2. SPI configuration (INTIDs >= 32):
     *    - all group 1 non-secure
     *    - priority 0xA0 (middle)
     *    - level-sensitive (ICFGR = 0 for each 2-bit field)
     *    - routed to affinity 0 (aff3.2.1.0 = 0) via IROUTER
     *
     * Skip the SGI/PPI range [0..31] — those live in the redistributor's
     * SGI frame and are configured separately.
     */
    for (uint32_t i = 32; i <= s_max_spi; i += 32) {
        mmio_w32(s_dist_base, GICD_IGROUPR(i / 32), 0xFFFFFFFFU);
        mmio_w32(s_dist_base, GICD_ICENABLER(i / 32), 0xFFFFFFFFU);
        mmio_w32(s_dist_base, GICD_ICPENDR(i / 32), 0xFFFFFFFFU);
    }
    for (uint32_t i = 32; i <= s_max_spi; i += 4) {
        mmio_w32(s_dist_base, GICD_IPRIORITYR(i / 4), 0xA0A0A0A0U);
    }
    for (uint32_t i = 32; i <= s_max_spi; i += 16) {
        mmio_w32(s_dist_base, GICD_ICFGR(i / 16), 0);
    }
    for (uint32_t i = 32; i <= s_max_spi; i++) {
        /* Route to affinity 0 (CPU 0). Bit 31 (IRM) = 0 → use explicit
         * aff3.aff2.aff1.aff0. */
        mmio_w64(s_dist_base, GICD_IROUTER(i), 0);
    }

    /* 3. Enable distributor with ARE_NS + Group 1 NS. ARE is required
     *    in GICv3; GICD_ITARGETSR is RES0 once ARE is set. */
    mmio_w32(s_dist_base, GICD_CTLR,
             GICD_CTLR_ARE_NS | GICD_CTLR_EnableGrp1NS);
    gicd_wait_rwp();

    /* 4. Wake this CPU's redistributor. Must happen before any ICC_*
     *    system register traps will be allowed to function. */
    gicr_wake_bsp();

    /* 5. SGI/PPI configuration in the redistributor SGI frame:
     *    - all group 1 NS
     *    - priority 0xA0
     *    - disable everything initially; individual enables come from
     *      gic_v3_enable_irq() / gic_v3_timer_init().
     */
    mmio_w32(s_sgi_base, GICR_IGROUPR0, 0xFFFFFFFFU);
    mmio_w32(s_sgi_base, GICR_ICENABLER0, 0xFFFFFFFFU);
    mmio_w32(s_sgi_base, GICR_ICPENDR0, 0xFFFFFFFFU);
    for (uint32_t i = 0; i < 32; i += 4) {
        mmio_w32(s_sgi_base, GICR_IPRIORITYR(i / 4), 0xA0A0A0A0U);
    }
    /* Leave ICFGR1 (PPI config) as default — timer PPI 30 is set up
     * in gic_v3_timer_init(). */

    /* 6. Enable system-register access. SRE=1, DIB=1 (disable IRQ
     *    bypass), DFB=1 (disable FIQ bypass). */
    write_icc_sre_el1(0x7);

    /* 7. Priority mask: allow everything. */
    write_icc_pmr_el1(0xFF);

    /* 8. Binary point: no group preemption. */
    write_icc_bpr1_el1(0);

    /* 9. Enable Group 1 IRQs. */
    write_icc_igrpen1_el1(1);
}

void
gic_v3_enable_irq(uint32_t irq)
{
    if (irq < 32) {
        /* SGI or PPI: configure in the redistributor SGI frame. */
        mmio_w32(s_sgi_base, GICR_ISENABLER0, 1U << irq);
    } else {
        /* SPI: configure in the distributor. */
        mmio_w32(s_dist_base, GICD_ISENABLER(irq / 32),
                 1U << (irq % 32));
        gicd_wait_rwp();
    }
}

uint32_t
gic_v3_ack_irq(void)
{
    /* ICC_IAR1_EL1 returns INTID in bits [23:0]. */
    return (uint32_t)(read_icc_iar1_el1() & 0xFFFFFFU);
}

void
gic_v3_eoi(uint32_t irq)
{
    write_icc_eoir1_el1(irq);
}

void
gic_v3_timer_init(void)
{
    /* The non-secure EL1 physical timer (CNTP) is PPI 30 on GICv3.
     * PPIs live in the redistributor SGI frame and default to
     * level-sensitive in GICR_ICFGR1 — which is what we want for the
     * generic timer. All we need here is to enable the interrupt. */
    gic_v3_enable_irq(30);
}
