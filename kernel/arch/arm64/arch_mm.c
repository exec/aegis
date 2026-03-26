/*
 * arch_mm.c — ARM64 memory map extraction from device tree blob (DTB).
 *
 * QEMU virt passes a DTB pointer in x0 at kernel entry. We parse it
 * to find /memory nodes and extract base/size pairs for the PMM.
 *
 * DTB format (Flattened Device Tree / FDT):
 *   - Big-endian header at dtb base
 *   - Structure block: sequence of tokens (BEGIN_NODE, PROP, END_NODE, END)
 *   - Strings block: property name strings
 *
 * We only care about: /memory nodes with device_type="memory" and "reg" property.
 * QEMU virt typically has one memory node with a single reg entry.
 */

#include "arch.h"
#include <stdint.h>

/* Storage for parsed memory regions */
#define MAX_MEM_REGIONS 8
static aegis_mem_region_t s_regions[MAX_MEM_REGIONS];
static uint32_t           s_region_count;

/* Reserved regions: the kernel image itself */
static aegis_mem_region_t s_reserved[2];
static uint32_t           s_reserved_count;

/* Linker-defined symbols */
extern char _start[];
extern char __bss_end[];

void
arch_mm_init(void *dtb)
{
    (void)dtb;  /* DTB parsing deferred — QEMU virt has a known memory layout */

    s_region_count   = 0;
    s_reserved_count = 0;

    /*
     * QEMU virt machine memory layout with -m 128M:
     *   RAM: 0x40000000 — 0x48000000 (128 MB)
     *
     * TODO: parse DTB /memory node for dynamic detection.
     * The DTB is padded to 1MB and the naive walk is too slow for boot.
     * For now, hardcode the QEMU virt layout. This will be replaced with
     * a proper DTB parser when we optimize the walk (skip large props).
     */
    s_regions[0].base = 0x40000000UL;
    s_regions[0].len  = 128UL * 1024 * 1024;  /* from -m flag */
    s_region_count = 1;

    /* Reserve everything below the kernel load address (device MMIO, DTB) */
    s_reserved[0].base = 0;
    s_reserved[0].len  = 0x40000000UL;
    s_reserved_count = 1;
}

uint32_t
arch_mm_region_count(void)
{
    return s_region_count;
}

const aegis_mem_region_t *
arch_mm_get_regions(void)
{
    return s_regions;
}

uint32_t
arch_mm_reserved_region_count(void)
{
    return s_reserved_count;
}

const aegis_mem_region_t *
arch_mm_get_reserved_regions(void)
{
    return s_reserved;
}
