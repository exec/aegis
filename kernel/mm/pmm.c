#include "pmm.h"
#include "arch.h"     /* aegis_mem_region_t, arch_mm_region_count/get_regions */
#include "printk.h"
#include <stdint.h>
#include <stddef.h>
/* Note: -nostdinc blocks string.h even from GCC freestanding headers.
 * Use a simple loop instead of memset for bitmap initialization.
 * printk has no format support; use local helpers to build strings. */

/* --------------------------------------------------------------------------
 * String formatting helpers (no libc available)
 * -------------------------------------------------------------------------- */

/* Write decimal representation of v into buf (must be at least 21 bytes).
 * Returns pointer past the last character written (not NUL-terminated). */
static char *u64_to_dec(char *buf, uint64_t v)
{
    char tmp[20];
    int  n = 0;
    if (v == 0) {
        *buf++ = '0';
        return buf;
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    /* tmp holds digits in reverse order */
    for (int i = n - 1; i >= 0; i--)
        *buf++ = tmp[i];
    return buf;
}

/* Append a NUL-terminated literal string src into dst; return ptr past end. */
static char *append_str(char *dst, const char *src)
{
    while (*src)
        *dst++ = *src++;
    return dst;
}

/* Bitmap covers 4GB of physical address space (1M pages × 1 bit = 128KB). */
#define PMM_MAX_PAGES (4ULL * 1024 * 1024 * 1024 / PAGE_SIZE)

/* 0 = free, 1 = allocated. Start fully reserved; pmm_init frees usable pages. */
static uint8_t pmm_bitmap[PMM_MAX_PAGES / 8];

static uint64_t s_total_usable_bytes;

/* _kernel_end is exported by the linker script after .bss.
 * The bitmap array itself is in .bss, so _kernel_end is after it. */
extern char _kernel_end[];

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static void pmm_free_region(uint64_t base, uint64_t len)
{
    /* Align base up to PAGE_SIZE, len down to a page boundary. */
    uint64_t start = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (start >= base + len)
        return;
    uint64_t end = (base + len) & ~(PAGE_SIZE - 1);

    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
        uint64_t idx = addr / PAGE_SIZE;
        if (idx < PMM_MAX_PAGES)
            pmm_bitmap[idx / 8] &= (uint8_t)~(1U << (idx % 8));
    }
}

static void pmm_reserve_region(uint64_t base, uint64_t len)
{
    /* Align base down, end up — reserve conservatively. */
    uint64_t start = base & ~(PAGE_SIZE - 1);
    uint64_t end   = (base + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
        uint64_t idx = addr / PAGE_SIZE;
        if (idx < PMM_MAX_PAGES)
            pmm_bitmap[idx / 8] |= (uint8_t)(1U << (idx % 8));
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void pmm_init(void)
{
    /* Step 1: start with everything reserved (safe default) */
    for (uint64_t i = 0; i < sizeof(pmm_bitmap); i++)
        pmm_bitmap[i] = 0xFF;

    /* Step 2: mark usable RAM as free */
    uint32_t                  nregions = arch_mm_region_count();
    const aegis_mem_region_t *regions  = arch_mm_get_regions();

    uint64_t total_bytes = 0;
    for (uint32_t i = 0; i < nregions; i++) {
        pmm_free_region(regions[i].base, regions[i].len);
        total_bytes += regions[i].len;
    }

    s_total_usable_bytes = total_bytes;

    /* Step 3: re-reserve arch platform ranges (first 1MB on x86) */
    uint32_t                  nreserved = arch_mm_reserved_region_count();
    const aegis_mem_region_t *reserved  = arch_mm_get_reserved_regions();

    for (uint32_t i = 0; i < nreserved; i++)
        pmm_reserve_region(reserved[i].base, reserved[i].len);

    /* Step 4: reserve the kernel image + bitmap (bitmap is in .bss,
     * inside this range).  The kernel image spans from PHYS_BASE to
     * PHYS_BASE + kernel_size, where kernel_size = _kernel_end_VA - VIRT_BASE.
     * Cast through uintptr_t first: direct pointer-to-uint64_t cast
     * may warn under C99 §7.18.1.4. */
    pmm_reserve_region(ARCH_KERNEL_PHYS_BASE,
                       (uint64_t)(uintptr_t)_kernel_end - ARCH_KERNEL_VIRT_BASE);

    /* Step 5: report — derived from raw multiboot2 usable bytes (before
     * our own reservations) so this line stays stable as the kernel grows. */
    uint64_t mb = total_bytes / (1024 * 1024);
    {
        char    buf[96];
        char   *p = buf;
        p = append_str(p, "[PMM] OK: ");
        p = u64_to_dec(p, mb);
        p = append_str(p, "MB usable across ");
        p = u64_to_dec(p, (uint64_t)nregions);
        p = append_str(p, " regions\n");
        *p = '\0';
        printk("%s", buf);
    }
}

uint64_t pmm_alloc_page(void)
{
    /* Linear scan: find first byte that is not 0xFF, then find the
     * first clear bit in that byte. O(n) is acceptable for Phase 2. */
    for (uint64_t i = 0; i < PMM_MAX_PAGES / 8; i++) {
        if (pmm_bitmap[i] == 0xFF)
            continue;
        for (int bit = 0; bit < 8; bit++) {
            if (!(pmm_bitmap[i] & (1U << bit))) {
                pmm_bitmap[i] |= (uint8_t)(1U << bit);
                return (i * 8 + (uint64_t)bit) * PAGE_SIZE;
            }
        }
    }
    printk("[PMM] WARN: out of physical memory\n");
    return 0;   /* OOM — 0 is always reserved, unambiguous sentinel */
}

void pmm_free_page(uint64_t addr)
{
    if (addr & (PAGE_SIZE - 1)) {
        char  buf[80];
        char *p = buf;
        p = append_str(p, "[PMM] FAIL: pmm_free_page called with unaligned addr ");
        p = u64_to_dec(p, addr);
        p = append_str(p, "\n");
        *p = '\0';
        printk("%s", buf);
        for (;;) {} /* NOTE: interrupts not disabled; harden when ISRs exist */
    }
    uint64_t idx = addr / PAGE_SIZE;
    if (idx >= PMM_MAX_PAGES) {
        /* Silently skip: address is outside PMM-managed RAM (e.g. MMIO like
         * framebuffer physical addresses mapped into user space by sys_fb_map).
         * These pages were never allocated by PMM and must not be freed. */
        return;
    }
    uint8_t  bit = (uint8_t)(1U << (idx % 8));
    if (!(pmm_bitmap[idx / 8] & bit)) {
        /* Page not allocated — could be MMIO mapped into user space (e.g.
         * framebuffer), or an actual double-free bug. Silently skip rather
         * than panicking, since MMIO pages are legitimately freed during
         * vmm_free_user_pml4 after process exit. */
        return;
    }
    pmm_bitmap[idx / 8] &= (uint8_t)~bit;
}

uint64_t
pmm_total_pages(void)
{
    return s_total_usable_bytes / PAGE_SIZE;
}

uint64_t
pmm_free_pages(void)
{
    uint64_t free_count = 0;
    for (uint64_t i = 0; i < PMM_MAX_PAGES / 8; i++) {
        uint8_t inv = (uint8_t)~pmm_bitmap[i];
        while (inv) {
            free_count++;
            inv &= (inv - 1);  /* clear lowest set bit */
        }
    }
    return free_count;
}
