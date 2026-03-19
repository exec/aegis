# Phase 2 — Physical Memory Manager Design Spec

**Date:** 2026-03-19
**Status:** Approved
**Phase:** 2 of N

---

## Goal

Implement a bitmap-based physical memory manager that parses the multiboot2
memory map, tracks free 4KB pages, and exposes `pmm_alloc_page()` /
`pmm_free_page()` to the rest of the kernel. After this phase, `make test`
still exits 0 with one additional line in the expected output.

---

## Scope

- **In scope:** multiboot2 memory map parsing, bitmap allocator, kernel and
  BIOS reservation, PMM status line in serial output.
- **Out of scope:** virtual memory mapping, higher-half remapping (Phase 3),
  multi-page contiguous allocation (future buddy allocator), DMA zones,
  NUMA awareness, memory hotplug.

**Single-page only.** `pmm_alloc_page()` allocates exactly one 4KB page.
Multi-page contiguous allocation is deferred to a future buddy-allocator
upgrade. The interface is designed so that upgrade is a drop-in internal
replacement with no API change.

---

## Files

| File | Action | Responsibility |
|------|--------|----------------|
| `kernel/mm/pmm.h` | Create | Public PMM interface |
| `kernel/mm/pmm.c` | Create | Bitmap allocator (arch-agnostic) |
| `kernel/arch/x86_64/arch_mm.c` | Create | Multiboot2 memory map parser |
| `kernel/arch/x86_64/arch.h` | Modify | Add `aegis_mem_region_t`, `arch_mm_init`, query API |
| `kernel/core/main.c` | Modify | Add `arch_mm_init()` + `pmm_init()` to boot sequence |
| `tools/linker.ld` | Modify | Export `_kernel_end` after `.bss` |
| `tests/expected/boot.txt` | Modify | Add `[PMM] OK` line (RED before implementation) |
| `.claude/CLAUDE.md` | Modify | Update build status; note multi-page deferral |

---

## Architecture

The arch layer handles all multiboot2 parsing. The PMM is fully
arch-agnostic — it knows nothing about multiboot2 tags or x86 memory
conventions. `main.c` sequences the two:

```
arch_mm_init(mb_info)   →   arch_mm.c walks multiboot2 tags,
                             stores usable regions in static table

pmm_init()              →   pmm.c queries arch_mm_get_regions(),
                             sets up bitmap, reserves kernel memory,
                             prints [PMM] OK line
```

Dependency direction: `pmm.c` → `arch.h` (for region query). `arch_mm.c`
has no dependency on `pmm.c`. This keeps `kernel/mm/` arch-agnostic at the
source level while allowing it to query arch state through the established
`arch.h` boundary.

---

## Section 1: Arch Layer — Multiboot2 Parser

### arch.h additions

```c
/* Physical memory region (usable RAM, from multiboot2 type=1 entries). */
typedef struct {
    uint64_t base;
    uint64_t len;
} aegis_mem_region_t;

/* Parse multiboot2 memory map tags. Must be called before pmm_init().
 * Safe to call with the raw mb_info physical pointer in Phase 2 because
 * virtual == physical under identity mapping. The VMM phase must remap
 * this pointer before the higher-half switch. */
void arch_mm_init(void *mb_info);

/* Number of usable (type=1) regions found by arch_mm_init(). */
uint32_t arch_mm_region_count(void);

/* Pointer to the internal usable-region table. Valid after arch_mm_init(). */
const aegis_mem_region_t *arch_mm_get_regions(void);
```

### arch_mm.c implementation

```c
#include "arch.h"
#include <stdint.h>
#include <stddef.h>

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
    uint32_t type;       /* = 6 */
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
} mb2_mmap_tag_t;

/* Multiboot2 memory map entry */
typedef struct {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;       /* 1 = available */
    uint32_t reserved;
} mb2_mmap_entry_t;

#define MB2_TAG_MMAP   6
#define MB2_MEM_AVAIL  1
#define MAX_REGIONS    32

static aegis_mem_region_t regions[MAX_REGIONS];
static uint32_t           region_count = 0;

void arch_mm_init(void *mb_info)
{
    /* SAFETY: mb_info is a physical address equal to the virtual address
     * under Phase 2 identity mapping. Casting to a pointer is safe here.
     * The VMM phase must remap this before switching to higher-half. */
    const mb2_info_t *info = (const mb2_info_t *)mb_info;
    const uint8_t *p = (const uint8_t *)mb_info + sizeof(mb2_info_t);
    const uint8_t *end = (const uint8_t *)mb_info + info->total_size;

    while (p < end) {
        const mb2_tag_t *tag = (const mb2_tag_t *)p;

        if (tag->type == 0)   /* end tag */
            break;

        if (tag->type == MB2_TAG_MMAP) {
            const mb2_mmap_tag_t *mmap = (const mb2_mmap_tag_t *)p;
            const uint8_t *entry_p = p + sizeof(mb2_mmap_tag_t);
            const uint8_t *entry_end = p + mmap->size;

            while (entry_p < entry_end && region_count < MAX_REGIONS) {
                const mb2_mmap_entry_t *e = (const mb2_mmap_entry_t *)entry_p;
                if (e->type == MB2_MEM_AVAIL) {
                    regions[region_count].base = e->base_addr;
                    regions[region_count].len  = e->length;
                    region_count++;
                }
                entry_p += mmap->entry_size;
            }
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
```

---

## Section 2: PMM — Bitmap Allocator

### pmm.h

```c
#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define PAGE_SIZE 4096UL

/* Initialise the physical memory manager.
 * Requires arch_mm_init() to have been called first.
 * Prints [PMM] OK on success; panics on failure. */
void pmm_init(void);

/* Allocate one 4KB physical page.
 * Returns the physical address of the page, or 0 on OOM.
 * Address 0 is always reserved, so 0 is unambiguous as an error sentinel.
 *
 * NOTE: single-page (4KB) allocation only. Multi-page contiguous allocation
 * is deferred to a future buddy-allocator upgrade. The upgrade will replace
 * the internals of pmm.c without changing this header. */
uint64_t pmm_alloc_page(void);

/* Free a page previously returned by pmm_alloc_page().
 * addr must be PAGE_SIZE-aligned. Panics on double-free or bad address. */
void pmm_free_page(uint64_t addr);

#endif /* PMM_H */
```

### pmm.c structure

```c
#include "pmm.h"
#include "arch.h"     /* aegis_mem_region_t, arch_mm_region_count/get_regions */
#include "printk.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>   /* memset — from GCC freestanding headers */

/* Bitmap covers 4GB of physical address space (1M pages × 1 bit = 128KB). */
#define PMM_MAX_PAGES (4ULL * 1024 * 1024 * 1024 / PAGE_SIZE)

/* 0 = free, 1 = allocated. */
static uint8_t pmm_bitmap[PMM_MAX_PAGES / 8];

/* _kernel_end is exported by the linker script after .bss. */
extern char _kernel_end[];
```

### pmm_init() sequence

```
1. memset(pmm_bitmap, 0xFF, sizeof(pmm_bitmap))
       — start with everything reserved (safe default for unknown memory)

2. for each region in arch_mm_get_regions():
       pmm_free_region(region.base, region.len)
       — mark usable RAM as free

3. pmm_reserve(0x0,       0x100000)
       — first 1MB: BIOS data areas, VGA framebuffer, ISA ROMs

4. pmm_reserve(0x100000,  _kernel_end - 0x100000)
       — kernel image + bitmap (bitmap is in .bss, inside this range)

5. count total usable bytes (sum of usable region lengths before step 3/4)
   count number of usable regions
   printk("[PMM] OK: NMB usable across N regions\n")
```

The MB count and region count are derived from the **raw multiboot2 usable
regions** (before our own reservations). This makes the `[PMM] OK` line
stable as the kernel image grows — boot.txt does not need updating when code
is added.

### Static helpers (internal to pmm.c)

```c
static void pmm_free_region(uint64_t base, uint64_t len);
   /* Align base up to PAGE_SIZE, len down; clear corresponding bits. */

static void pmm_reserve(uint64_t base, uint64_t len);
   /* Set bits for [base, base+len). Used for first-1MB and kernel. */
```

### pmm_alloc_page()

Linear scan: find first byte in `pmm_bitmap` that is not `0xFF`, then find
the first clear bit within that byte, set it, return `page_index * PAGE_SIZE`.
Returns `0` on OOM. O(n) scan is acceptable for Phase 2; replaced when the
buddy allocator arrives.

### pmm_free_page()

Clear bit at `addr / PAGE_SIZE`. Call `printk` + halt (panic) on:
- `addr` not PAGE_SIZE-aligned
- double-free (bit already clear)

---

## Section 3: Linker Script Change

Add `_kernel_end` after `.bss`:

```ld
    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    }
    _kernel_end = .;
```

This symbol is the authoritative end of the kernel image including BSS and
the PMM bitmap array. `pmm_reserve()` uses it as the upper bound of the
kernel reservation.

---

## Section 4: main.c Boot Sequence

```c
arch_init();           /* serial + VGA                          */
arch_mm_init(mb_info); /* parse multiboot2 memory map           */
pmm_init();            /* bitmap allocator — prints [PMM] OK    */
cap_init();            /* capability stub                        */
printk("[AEGIS] System halted.\n");
arch_debug_exit(0x01);
```

`mb_magic` continues to be ignored in Phase 2. It will be validated against
`0x36D76289` in the PMM phase or the first phase that needs to abort on
bad boot (noted as a Phase 3 concern).

---

## Section 5: Test Harness

### tests/expected/boot.txt (after Phase 2)

```
[SERIAL] OK: COM1 initialized at 115200 baud
[VGA] OK: text mode 80x25
[PMM] OK: 127MB usable across 2 regions
[CAP] OK: capability subsystem reserved
[AEGIS] System halted.
```

The `[PMM] OK` line is added to `boot.txt` **before** `pmm_init()` is
implemented — that is the failing RED test. The numbers (127MB, 2 regions)
are the expected values for QEMU `-m 128M` with SeaBIOS. If the first run
shows different numbers, `boot.txt` is updated to match and the deviation
is documented.

### TDD order

1. Update `boot.txt` with the PMM line → `make test` fails (RED)
2. Implement `arch_mm.c` + `pmm.c` → `make test` passes (GREEN)
3. Review → refactor if needed

---

## Section 6: Known Constraints and Future Work

### Multi-page allocation (deferred)

Phase 2 allocates only single 4KB pages. When the VMM phase requires
contiguous physical ranges (e.g., for initial page tables), the PMM
internals will be upgraded to a buddy allocator. The public interface
(`pmm_alloc_page` / `pmm_free_page`) does not change — the upgrade
is purely internal.

### Identity mapping assumption

`arch_mm_init()` dereferences `mb_info` as a direct pointer. This is safe
in Phase 2 because virtual == physical under the identity mapping established
in `boot.asm`. The VMM phase must remap `mb_info` before enabling higher-half
virtual addressing. A comment in `main.c` already marks this assumption.

### Bitmap scan performance

`pmm_alloc_page()` uses a linear scan (O(n)). For a 128KB bitmap covering 4GB,
worst-case scan is 131072 iterations. Acceptable for early kernel boot where
no performance-critical code depends on fast physical allocation. The buddy
allocator upgrade makes this O(log n).

### memset availability

`pmm.c` uses `memset` from GCC's freestanding headers. This is available
under `-ffreestanding` with the current CFLAGS. No libc dependency.

---

## Decisions Log

| Question | Decision | Reason |
|----------|----------|--------|
| Bitmap vs free-list vs buddy | Bitmap | Simplest, adequate for Phase 2, clean upgrade path |
| Single-page vs multi-page API | Single-page only | YAGNI; buddy allocator adds multi-page later |
| Bitmap location | Static BSS array (128KB) | No placement arithmetic; covered by kernel reservation |
| Arch boundary | arch_mm.c parses multiboot2; pmm.c pulls regions via arch.h | Keeps kernel/mm/ arch-agnostic |
| MB count in OK line | Raw usable bytes from multiboot2 (before our reservations) | Stable as kernel grows |
| OOM sentinel | Return 0 (address 0 always reserved) | Unambiguous without a separate error type |
