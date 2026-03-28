/*
 * lapic.c — Local APIC driver for x86-64
 *
 * Maps the LAPIC MMIO page (typically 0xFEE00000) into kernel VA and provides
 * EOI, IPI, and ID accessors. BSP calls lapic_init(); APs call lapic_init_ap()
 * which reuses the BSP's MMIO mapping (same physical address, per-CPU register
 * file selected by hardware).
 */

#include "lapic.h"
#include "arch.h"
#include "../../mm/vmm.h"
#include "../../mm/kva.h"
#include "../../core/printk.h"

#include <stdint.h>

/* IA32_APIC_BASE MSR */
#define MSR_APIC_BASE       0x1B

/* LAPIC register offsets (byte offsets from base) */
#define LAPIC_ID            0x020
#define LAPIC_TPR           0x080
#define LAPIC_EOI           0x0B0
#define LAPIC_SVR           0x0F0
#define LAPIC_ICR_LOW       0x300
#define LAPIC_ICR_HIGH      0x310

/* SVR bits */
#define SVR_ENABLE          (1u << 8)
#define SVR_SPURIOUS_VEC    0xFF

/* ICR bits */
#define ICR_DELIVERY_STATUS (1u << 12)
#define ICR_SHORTHAND_SHIFT 18

static volatile uint32_t *s_lapic_base;   /* kernel VA of LAPIC MMIO page */
static int                s_active;       /* 1 after successful init       */

/* --------------------------------------------------------------------------
 * MMIO helpers — all accesses are 32-bit aligned register reads/writes.
 * -------------------------------------------------------------------------- */

static inline uint32_t
lapic_read(uint32_t offset)
{
    return s_lapic_base[offset / 4];
}

static inline void
lapic_write(uint32_t offset, uint32_t val)
{
    s_lapic_base[offset / 4] = val;
}

/* --------------------------------------------------------------------------
 * MSR helpers
 * -------------------------------------------------------------------------- */

static inline uint64_t
rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* --------------------------------------------------------------------------
 * Internal: configure SVR + TPR (shared between BSP and AP init)
 * -------------------------------------------------------------------------- */

static void
lapic_configure(void)
{
    /* Enable LAPIC via SVR: set enable bit + spurious vector 0xFF */
    lapic_write(LAPIC_SVR, SVR_ENABLE | SVR_SPURIOUS_VEC);

    /* Clear task priority — accept all interrupts */
    lapic_write(LAPIC_TPR, 0);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/*
 * lapic_init — BSP initialization.
 *
 * Reads IA32_APIC_BASE MSR to find the LAPIC physical address, maps one
 * 4KB MMIO page into kernel VA (uncached), and configures SVR + TPR.
 */
void
lapic_init(void)
{
    uint64_t msr_val  = rdmsr(MSR_APIC_BASE);
    uint64_t phys     = msr_val & 0xFFFFF000ULL;  /* bits [35:12] */

    /* Allocate a kernel VA page, then remap it to the LAPIC MMIO address.
     * Pattern: kva gives us a PMM-backed page; we unmap that backing and
     * remap to the device physical address with uncacheable flags. */
    void *va = kva_alloc_pages(1);
    if (!va) {
        printk("[LAPIC] FAIL: kva_alloc_pages returned NULL\n");
        return;
    }
    vmm_unmap_page((uintptr_t)va);
    vmm_map_page((uintptr_t)va, phys,
                 VMM_FLAG_WRITABLE | VMM_FLAG_WC | VMM_FLAG_UCMINUS);

    s_lapic_base = (volatile uint32_t *)va;

    lapic_configure();

    s_active = 1;

    printk("[LAPIC] OK: id=%u base=0x%lx\n",
           (unsigned)lapic_id(), phys);
}

/*
 * lapic_init_ap — AP initialization.
 *
 * Reuses the BSP's MMIO mapping (the LAPIC physical address is the same for
 * all cores; hardware selects the per-CPU register file internally).
 * Configures SVR + TPR for this AP.
 */
void
lapic_init_ap(void)
{
    lapic_configure();
}

/*
 * lapic_id — return this CPU's LAPIC ID (bits [31:24] of the ID register).
 */
uint8_t
lapic_id(void)
{
    return (uint8_t)(lapic_read(LAPIC_ID) >> 24);
}

/*
 * lapic_eoi — signal end-of-interrupt to the local APIC.
 */
void
lapic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

/*
 * lapic_send_ipi — send a fixed IPI to a specific APIC ID.
 *
 * Waits for the ICR delivery-status bit to clear before writing.
 */
void
lapic_send_ipi(uint8_t dest_apic_id, uint8_t vector)
{
    /* Wait for previous IPI to complete */
    while (lapic_read(LAPIC_ICR_LOW) & ICR_DELIVERY_STATUS)
        arch_pause();

    /* Set destination in ICR_HIGH bits [31:24] */
    lapic_write(LAPIC_ICR_HIGH, (uint32_t)dest_apic_id << 24);

    /* Fire: fixed delivery, edge, assert, physical destination */
    lapic_write(LAPIC_ICR_LOW, (uint32_t)vector);
}

/*
 * lapic_send_ipi_all_excl_self — broadcast IPI to all CPUs except this one.
 *
 * Uses shorthand 11b (all-excluding-self) in ICR_LOW bits [19:18].
 */
void
lapic_send_ipi_all_excl_self(uint8_t vector)
{
    while (lapic_read(LAPIC_ICR_LOW) & ICR_DELIVERY_STATUS)
        arch_pause();

    /* Shorthand = 11 (all excluding self), no destination needed */
    lapic_write(LAPIC_ICR_LOW,
                (uint32_t)vector | (3u << ICR_SHORTHAND_SHIFT));
}

/*
 * lapic_active — return 1 if LAPIC has been initialized, 0 otherwise.
 */
int
lapic_active(void)
{
    return s_active;
}
