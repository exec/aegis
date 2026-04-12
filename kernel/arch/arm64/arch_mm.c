/*
 * arch_mm.c — ARM64 memory map from DTB (device tree blob).
 *
 * Parses the /memory node's "reg" property to discover RAM regions.
 * Uses the physical DTB address (passed in x0 by firmware, before MMU).
 */

#include "arch.h"
#include "printk.h"
#include <stdint.h>

#define MAX_MEM_REGIONS 8
static aegis_mem_region_t s_regions[MAX_MEM_REGIONS];
static uint32_t           s_region_count;
static aegis_mem_region_t s_reserved[2];
static uint32_t           s_reserved_count;

/* Forward decl — from gic.c */
void gic_set_version(int version, uint64_t dist_pa, uint64_t redist_or_cpu_pa);

/* Forward decl — DTB GIC detection helper. */
static void detect_gic_version(const uint8_t *dtb, uint32_t totalsize);

/* Big-endian → host */
static inline uint32_t be32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  | (uint32_t)b[3];
}
static inline uint64_t be64(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return ((uint64_t)be32(b) << 32) | (uint64_t)be32(b + 4);
}

/* FDT tokens */
#define FDT_MAGIC      0xD00DFEED
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4
#define FDT_END        9

/* Simple string prefix match */
static int
starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

void
arch_mm_init(void *dtb)
{
    s_region_count = 0;
    s_reserved_count = 0;

    const uint8_t *base = (const uint8_t *)dtb;
    if (be32(base) != FDT_MAGIC) {
        /* No valid DTB — fall back to 128MB hardcoded */
        s_regions[0].base = 0x40000000UL;
        s_regions[0].len  = 128UL * 1024 * 1024;
        s_region_count = 1;
        goto done;
    }

    uint32_t totalsize = be32(base + 4);
    /* Cap at 8KB — struct block (~7KB) + strings (~1KB).
     * The full 1MB DTB is mostly zero-padding. */
    if (totalsize > 8192) totalsize = 8192;

    /* Copy DTB into a stack buffer — device memory reads are very slow. */
    uint8_t dtb_copy[8192];
    {
        const volatile uint8_t *src = (const volatile uint8_t *)base;
        uint32_t ci;
        for (ci = 0; ci < totalsize; ci++)
            dtb_copy[ci] = src[ci];
    }
    const uint8_t *d = dtb_copy;

    /* Detect GIC version from /intc node — see helper below. */
    detect_gic_version(d, totalsize);

    uint32_t struct_off  = be32(d + 8);
    uint32_t strings_off = be32(d + 12);
    const uint8_t *structs = d + struct_off;
    const char    *strings = (const char *)(d + strings_off);

    const uint8_t *p = structs;
    const uint8_t *end = d + totalsize;
    int in_memory = 0;
    int depth = 0, mem_depth = -1;

    while (p < end) {
        uint32_t tok = be32(p); p += 4;

        switch (tok) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)p;
            /* Skip name (NUL-terminated, 4-byte aligned) */
            uint32_t len = 0;
            while (name[len]) len++;
            p += (len + 4) & ~3U;
            depth++;
            if (starts_with(name, "memory")) {
                in_memory = 1;
                mem_depth = depth;
            }
            break;
        }
        case FDT_END_NODE:
            if (depth == mem_depth) {
                in_memory = 0;
                mem_depth = -1;
            }
            depth--;
            break;
        case FDT_PROP: {
            uint32_t prop_len = be32(p); p += 4;
            uint32_t name_off = be32(p); p += 4;
            const uint8_t *data = p;
            p += (prop_len + 3) & ~3U;  /* skip data, 4-byte aligned */

            if (in_memory && starts_with(strings + name_off, "reg")) {
                uint32_t i;
                for (i = 0; i + 16 <= prop_len && s_region_count < MAX_MEM_REGIONS; i += 16) {
                    uint64_t rbase = be64(data + i);
                    uint64_t rsize = be64(data + i + 8);
                    if (rsize > 0) {
                        s_regions[s_region_count].base = rbase;
                        s_regions[s_region_count].len  = rsize;
                        s_region_count++;
                    }
                }
                /* Found memory — no need to walk the rest of the DTB */
                goto walk_done;
            }
            break;
        }
        case FDT_NOP:
            break;
        case FDT_END:
            goto walk_done;
        default:
            goto walk_done;
        }
    }

walk_done:
    if (s_region_count == 0) {
        /* Fallback if DTB had no memory node */
        s_regions[0].base = 0x40000000UL;
        s_regions[0].len  = 128UL * 1024 * 1024;
        s_region_count = 1;
    }

done:
    /* Reserve everything below kernel load address */
    s_reserved[0].base = 0;
    s_reserved[0].len  = 0x40000000UL;
    s_reserved_count = 1;
}

uint32_t arch_mm_region_count(void)            { return s_region_count; }
const aegis_mem_region_t *arch_mm_get_regions(void) { return s_regions; }
uint32_t arch_mm_reserved_region_count(void)   { return s_reserved_count; }
const aegis_mem_region_t *arch_mm_get_reserved_regions(void) { return s_reserved; }

/* ---------------------------------------------------------------------
 * GIC detection from DTB.
 *
 * Walks the DTB structure block looking for the interrupt-controller
 * node (matching name prefixes "intc", "interrupt-controller", or a
 * node whose compatible string is a known GIC string). Reads its
 * `compatible` + `reg` properties and calls gic_set_version().
 *
 * GICv2 reg layout (2 regions): [dist_base, dist_size,  cpu_base, cpu_size]
 * GICv3 reg layout (≥2 regions): [dist_base, dist_size, redist_base, redist_size, ...]
 *
 * #address-cells/#size-cells at the root are both 2 on every ARM64
 * DTB QEMU and the Pi produce, so each cell pair is 16 bytes.
 * --------------------------------------------------------------------- */

/* Returns 1 if `haystack` contains `needle` as a NUL-terminated
 * substring in the compatible string list (which is a sequence of
 * NUL-terminated strings packed together). */
static int
compat_contains(const char *list, uint32_t list_len, const char *needle)
{
    uint32_t i = 0;
    while (i < list_len) {
        const char *s = list + i;
        uint32_t slen = 0;
        while (i + slen < list_len && s[slen]) slen++;
        /* compare */
        const char *a = s;
        const char *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*b == 0 && *a == 0) return 1;
        i += slen + 1;
    }
    return 0;
}

static void
detect_gic_version(const uint8_t *d, uint32_t totalsize)
{
    uint32_t struct_off  = be32(d + 8);
    uint32_t strings_off = be32(d + 12);
    const uint8_t *structs = d + struct_off;
    const char    *strings = (const char *)(d + strings_off);

    const uint8_t *p = structs;
    const uint8_t *end = d + totalsize;

    /* Stack of whether we're currently inside an "interesting" node
     * (one whose name starts with "intc" or "interrupt-controller"). */
    int in_intc = 0;
    int intc_depth = -1;
    int depth = 0;

    /* Per-node scratch */
    int     have_compat = 0;
    const char *compat_data = 0;
    uint32_t compat_len = 0;
    int     have_reg = 0;
    const uint8_t *reg_data = 0;
    uint32_t reg_len = 0;

    while (p < end) {
        uint32_t tok = be32(p); p += 4;

        switch (tok) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)p;
            uint32_t len = 0;
            while (name[len]) len++;
            p += (len + 4) & ~3U;
            depth++;
            /* Pi and QEMU virt both name the node something like
             * "intc@8000000" or "interrupt-controller@...". Match the
             * two common prefixes. */
            if (!in_intc &&
                (starts_with(name, "intc") ||
                 starts_with(name, "interrupt-controller"))) {
                in_intc = 1;
                intc_depth = depth;
                have_compat = 0;
                have_reg = 0;
            }
            break;
        }
        case FDT_END_NODE: {
            if (in_intc && depth == intc_depth) {
                /* Evaluate what we found. */
                if (have_compat && have_reg && reg_len >= 32) {
                    int version = 0;
                    if (compat_contains(compat_data, compat_len, "arm,gic-v3") ||
                        compat_contains(compat_data, compat_len, "arm,gic-600")) {
                        version = 3;
                    } else if (
                        compat_contains(compat_data, compat_len, "arm,gic-400") ||
                        compat_contains(compat_data, compat_len, "arm,cortex-a15-gic") ||
                        compat_contains(compat_data, compat_len, "arm,cortex-a7-gic")) {
                        version = 2;
                    }
                    if (version != 0) {
                        uint64_t dist_pa   = be64(reg_data + 0);
                        /* reg_data layout (assuming 2/2 cells):
                         *   [0..8)  dist_base
                         *   [8..16) dist_size
                         *   [16..24) secondary_base (cpu or redist)
                         *   [24..32) secondary_size
                         */
                        uint64_t second_pa = be64(reg_data + 16);
                        gic_set_version(version, dist_pa, second_pa);
                    }
                }
                in_intc = 0;
                intc_depth = -1;
            }
            depth--;
            break;
        }
        case FDT_PROP: {
            uint32_t prop_len = be32(p); p += 4;
            uint32_t name_off = be32(p); p += 4;
            const uint8_t *data = p;
            p += (prop_len + 3) & ~3U;

            if (in_intc && depth == intc_depth) {
                const char *pname = strings + name_off;
                if (starts_with(pname, "compatible")) {
                    have_compat = 1;
                    compat_data = (const char *)data;
                    compat_len = prop_len;
                } else if (starts_with(pname, "reg")) {
                    have_reg = 1;
                    reg_data = data;
                    reg_len = prop_len;
                }
            }
            break;
        }
        case FDT_NOP:
            break;
        case FDT_END:
            return;
        default:
            return;
        }
    }
}
