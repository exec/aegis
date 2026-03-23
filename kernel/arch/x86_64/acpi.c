/* acpi.c — ACPI table parser (MCFG + MADT)
 *
 * Phase 19 scope: locate MCFG to get PCIe ECAM base, locate MADT for
 * future interrupt routing. No AML interpreter. No power management.
 */
#include "acpi.h"
#include "arch.h"
#include "printk.h"
#include <stdint.h>
#include <stddef.h>

uint64_t g_mcfg_base      = 0;
uint8_t  g_mcfg_start_bus = 0;
uint8_t  g_mcfg_end_bus   = 0;
int      g_madt_found     = 0;

/* Accessible physical range: identity map covers [0..4MB) only.
 * ACPI tables on QEMU -machine pc land in high RAM (~128MB), outside
 * this window. A future phase may extend this via vmm_map_page. */
#define ACPI_PHYS_MAX 0x400000UL

/* Map a physical address to a kernel-accessible virtual address.
 * SAFETY: the identity map [0..4MB) is still active at acpi_init() time
 * (vmm_teardown_identity() runs later). Addresses >= 4MB are not mapped;
 * phys_accessible() must be checked before calling phys_to_virt(). */
static int phys_accessible(uint64_t phys)
{
    return phys < ACPI_PHYS_MAX;
}

static void *phys_to_virt(uint64_t phys)
{
    return (void *)(uintptr_t)phys;
}

static int acpi_checksum(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint8_t sum = 0;
    uint32_t i;
    for (i = 0; i < len; i++)
        sum += p[i];
    return sum == 0;
}

static void parse_mcfg(const acpi_sdt_header_t *hdr)
{
    const uint8_t *p   = (const uint8_t *)hdr + sizeof(acpi_mcfg_t);
    const uint8_t *end = (const uint8_t *)hdr + hdr->length;

    while (p + sizeof(acpi_mcfg_alloc_t) <= end) {
        const acpi_mcfg_alloc_t *alloc = (const acpi_mcfg_alloc_t *)p;
        if (alloc->segment == 0 && g_mcfg_base == 0) {
            g_mcfg_base      = alloc->base_address;
            g_mcfg_start_bus = alloc->start_bus;
            g_mcfg_end_bus   = alloc->end_bus;
        }
        p += sizeof(acpi_mcfg_alloc_t);
    }
}

static void scan_table(uint64_t phys)
{
    const acpi_sdt_header_t *hdr;

    if (!phys_accessible(phys))
        return;     /* table outside identity-mapped window — skip */

    hdr = (const acpi_sdt_header_t *)phys_to_virt(phys);

    if (!acpi_checksum(hdr, hdr->length))
        return;

    if (__builtin_memcmp(hdr->signature, "MCFG", 4) == 0)
        parse_mcfg(hdr);
    else if (__builtin_memcmp(hdr->signature, "APIC", 4) == 0)
        g_madt_found = 1;
}

void acpi_init(void)
{
    uint64_t rsdp_phys = arch_get_rsdp_phys();

    /* No RSDP found in multiboot2 tags — legacy machine. */
    if (rsdp_phys == 0 || !phys_accessible(rsdp_phys)) {
        printk("[ACPI] OK: MADT parsed, no MCFG (legacy machine)\n");
        return;
    }

    {
        const acpi_rsdp_t *rsdp =
            (const acpi_rsdp_t *)phys_to_virt(rsdp_phys);

        if (__builtin_memcmp(rsdp->signature, "RSD PTR ", 8) != 0) {
            printk("[ACPI] FAIL: invalid RSDP signature\n");
            return;
        }

        if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
            /* ACPI 2.0+: use XSDT with 64-bit table pointers */
            uint64_t xsdt_phys = rsdp->xsdt_address;
            if (phys_accessible(xsdt_phys)) {
                const acpi_sdt_header_t *xsdt =
                    (const acpi_sdt_header_t *)phys_to_virt(xsdt_phys);
                uint32_t count =
                    (xsdt->length - sizeof(acpi_sdt_header_t)) / 8;
                const uint64_t *entries = (const uint64_t *)(
                    (const uint8_t *)xsdt + sizeof(acpi_sdt_header_t));
                uint32_t i;
                for (i = 0; i < count; i++)
                    scan_table(entries[i]);
            }
        } else {
            /* ACPI 1.0: use RSDT with 32-bit table pointers */
            uint64_t rsdt_phys = (uint64_t)rsdp->rsdt_address;
            if (phys_accessible(rsdt_phys)) {
                const acpi_sdt_header_t *rsdt =
                    (const acpi_sdt_header_t *)phys_to_virt(rsdt_phys);
                uint32_t count =
                    (rsdt->length - sizeof(acpi_sdt_header_t)) / 4;
                const uint32_t *entries = (const uint32_t *)(
                    (const uint8_t *)rsdt + sizeof(acpi_sdt_header_t));
                uint32_t i;
                for (i = 0; i < count; i++)
                    scan_table((uint64_t)entries[i]);
            }
        }
    }

    if (g_mcfg_base != 0)
        printk("[ACPI] OK: MCFG+MADT parsed\n");
    else
        printk("[ACPI] OK: MADT parsed, no MCFG (legacy machine)\n");
}
