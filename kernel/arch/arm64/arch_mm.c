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
