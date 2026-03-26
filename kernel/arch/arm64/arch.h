#ifndef ARCH_H
#define ARCH_H

#include <stdint.h>

/*
 * arch.h — ARM64 architecture interface.
 *
 * Provides the same API as kernel/arch/x86_64/arch.h so that
 * kernel/core/ and kernel/mm/ compile unchanged.
 */

/* Initialize arch-specific subsystems (UART). */
void arch_init(void);

/* No-op on ARM64 (no PAT MSR). */
static inline void arch_pat_init(void) {}

/* Shutdown — write to QEMU virt power device. */
void arch_debug_exit(unsigned char value);

/* Output primitives used by printk. */
extern int vga_available;
void serial_write_string(const char *s);
void vga_write_string(const char *s);

/* -------------------------------------------------------------------------
 * Physical memory interface
 * ------------------------------------------------------------------------- */

typedef struct {
    uint64_t base;
    uint64_t len;
} aegis_mem_region_t;

/* Parse device tree blob for memory regions. */
void arch_mm_init(void *dtb);

uint32_t                   arch_mm_region_count(void);
const aegis_mem_region_t  *arch_mm_get_regions(void);
uint32_t                   arch_mm_reserved_region_count(void);
const aegis_mem_region_t  *arch_mm_get_reserved_regions(void);

/* No ACPI RSDP on QEMU virt — always 0. */
static inline uint64_t arch_get_rsdp_phys(void) { return 0; }

/* No framebuffer at this stage. */
typedef struct {
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  type;
} arch_fb_info_t;

static inline int arch_get_fb_info(arch_fb_info_t *out) { (void)out; return 0; }

/* Highest canonical user-space virtual address (48-bit VA). */
#define USER_ADDR_MAX 0x0000FFFFFFFFFFFFUL

/* Physical base address of the kernel image.
 * QEMU virt loads at 0x40000000 (1GB). */
#define ARCH_KERNEL_PHYS_BASE 0x40000000UL

/* Virtual base = physical for now (MMU off). VMM phase will change this. */
#define ARCH_KERNEL_VIRT_BASE 0x40000000UL

/* -------------------------------------------------------------------------
 * Arch-portable helpers
 * ------------------------------------------------------------------------- */

static inline void arch_wmb(void) { __asm__ volatile("dmb st" ::: "memory"); }
static inline void arch_enable_irq(void)  { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
static inline void arch_disable_irq(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }
static inline void arch_halt(void) { __asm__ volatile("wfi" ::: "memory"); }
static inline void arch_wait_for_irq(void) {
    __asm__ volatile("msr daifclr, #2; wfi; msr daifset, #2" ::: "memory");
}

#endif /* ARCH_H */
