#include "arch.h"
#include <stdint.h>
#include <stddef.h>

/* --------------------------------------------------------------------------
 * Multiboot2 structures (local to this file)
 * -------------------------------------------------------------------------- */

/* Multiboot2 info header */
typedef struct {
    uint32_t total_size;
    uint32_t reserved;
} mb2_info_t;

/* Multiboot2 tag header */
typedef struct {
    uint32_t type;
    uint32_t size;
} mb2_tag_t;

/* Multiboot2 memory map tag (type 6) */
typedef struct {
    uint32_t type;          /* = 6 */
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
} mb2_mmap_tag_t;

/* Multiboot2 memory map entry */
typedef struct {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;          /* 1 = available */
    uint32_t reserved;
} mb2_mmap_entry_t;

/* Multiboot2 ACPI old tag (type 14) — contains RSDP v1 inline */
typedef struct {
    uint32_t type;    /* = 14 */
    uint32_t size;
    /* RSDP follows immediately: 20 bytes (v1) */
} mb2_acpi_old_tag_t;

/* Multiboot2 ACPI new tag (type 15) — contains RSDP v2 inline */
typedef struct {
    uint32_t type;    /* = 15 */
    uint32_t size;
    /* RSDP follows immediately: 36 bytes (v2) */
} mb2_acpi_new_tag_t;

#define MB2_TAG_MMAP   6
#define MB2_TAG_ACPI_OLD 14   /* ACPI 1.0 RSDP (32-bit, RSDT) */
#define MB2_TAG_ACPI_NEW 15   /* ACPI 2.0+ RSDP (64-bit, XSDT) */
#define MB2_MEM_AVAIL  1
#define MAX_REGIONS    32

/* --------------------------------------------------------------------------
 * Usable RAM regions (type=1 from multiboot2)
 * -------------------------------------------------------------------------- */

static aegis_mem_region_t regions[MAX_REGIONS];
static uint32_t           region_count = 0;
static uint64_t s_rsdp_phys = 0;

void arch_mm_init(void *mb_info)
{
    /* SAFETY: mb_info is a physical address equal to the virtual address
     * under Phase 2 identity mapping. Casting to a pointer is safe here.
     * The VMM phase must remap this before switching to higher-half. */
    const mb2_info_t *info = (const mb2_info_t *)mb_info;
    const uint8_t *p   = (const uint8_t *)mb_info + sizeof(mb2_info_t);
    const uint8_t *end = (const uint8_t *)mb_info + info->total_size;

    while (p < end) {
        const mb2_tag_t *tag = (const mb2_tag_t *)p;

        if (tag->type == 0)   /* end tag */
            break;

        if (tag->type == MB2_TAG_MMAP) {
            const mb2_mmap_tag_t *mmap = (const mb2_mmap_tag_t *)p;
            const uint8_t *ep  = p + sizeof(mb2_mmap_tag_t);
            const uint8_t *epe = p + mmap->size;

            while (ep < epe && region_count < MAX_REGIONS) {
                const mb2_mmap_entry_t *e = (const mb2_mmap_entry_t *)ep;
                if (e->type == MB2_MEM_AVAIL) {
                    regions[region_count].base = e->base_addr;
                    regions[region_count].len  = e->length;
                    region_count++;
                }
                ep += mmap->entry_size;
            }
        }

        if (tag->type == MB2_TAG_ACPI_NEW && s_rsdp_phys == 0) {
            /* ACPI 2.0+ RSDP: skip the 8-byte tag header, RSDP starts there.
             * We store the physical address of the RSDP structure itself. */
            s_rsdp_phys = (uint64_t)(uintptr_t)(p + sizeof(mb2_acpi_new_tag_t));
        }
        if (tag->type == MB2_TAG_ACPI_OLD && s_rsdp_phys == 0) {
            /* ACPI 1.0 RSDP fallback — only if no v2 tag found. */
            s_rsdp_phys = (uint64_t)(uintptr_t)(p + sizeof(mb2_acpi_old_tag_t));
        }

        /* Tags are 8-byte aligned */
        p += (tag->size + 7) & ~7U;
    }
}

uint32_t arch_mm_region_count(void)
{
    return region_count;
}

const aegis_mem_region_t *arch_mm_get_regions(void)
{
    return regions;
}

/* --------------------------------------------------------------------------
 * x86 reserved regions — all platform-specific addresses live here.
 * pmm.c calls this instead of hard-coding any platform address.
 * ARM64's arch_mm.c returns a different (or empty) table.
 * -------------------------------------------------------------------------- */

static const aegis_mem_region_t x86_reserved[] = {
    { 0x0UL, 0x100000UL },  /* first 1MB: BIOS data, VGA hole, ISA ROMs */
};

uint32_t arch_mm_reserved_region_count(void)
{
    return sizeof(x86_reserved) / sizeof(x86_reserved[0]);
}

const aegis_mem_region_t *arch_mm_get_reserved_regions(void)
{
    return x86_reserved;
}

uint64_t arch_get_rsdp_phys(void)
{
    return s_rsdp_phys;
}
