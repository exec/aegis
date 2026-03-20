#ifndef ARCH_H
#define ARCH_H

#include <stdint.h>   /* uint32_t etc. from GCC freestanding headers */

/*
 * arch.h — Architecture-neutral boundary for kernel/core/.
 *
 * Every architecture provides its own arch.h with this same interface.
 * The build system selects the right one via -Ikernel/arch/<target>.
 * kernel/core/ includes only "arch.h" — never any arch-specific header by name.
 *
 * Design debt (Phase 2): the output function declarations below
 * (serial_write_string, vga_write_string, vga_available) are x86-specific
 * in semantics and should migrate to a kernel/core/output.h abstraction
 * when an ARM64 port begins.
 */

/* Initialize all arch-specific subsystems (serial, VGA).
 * Must be the first call in kernel_main. */
void arch_init(void);

/* Signal QEMU isa-debug-exit device. QEMU exits with code (value << 1) | 1.
 * Writing 0x01 → QEMU exit code 3. No-op if device is absent. */
void arch_debug_exit(unsigned char value);

/*
 * Output primitives used by printk (implemented in serial.c / vga.c).
 * Declared here so kernel/core/printk.c can reach them via arch.h
 * without including serial.h or vga.h directly.
 */
extern int vga_available;              /* set to 1 by vga_init() */
void serial_write_string(const char *s);
void vga_write_string(const char *s);

/* -------------------------------------------------------------------------
 * Physical memory interface (Phase 2+)
 * ------------------------------------------------------------------------- */

/* A contiguous physical memory region. Used for both usable RAM and
 * arch-reserved ranges (BIOS holes, MMIO, ROM regions, etc.). */
typedef struct {
    uint64_t base;
    uint64_t len;
} aegis_mem_region_t;

/* Parse the arch-specific firmware memory map (multiboot2 on x86).
 * Must be called before any pmm_* function. */
void arch_mm_init(void *mb_info);

/* Usable RAM regions reported by firmware (multiboot2 type=1 entries).
 * Valid after arch_mm_init(). */
uint32_t                   arch_mm_region_count(void);
const aegis_mem_region_t  *arch_mm_get_regions(void);

/* Arch-reserved physical ranges that pmm_init() must never hand out
 * (BIOS data areas, VGA framebuffer hole, ISA ROMs, MMIO windows, etc.).
 * pmm_init() calls this instead of hard-coding platform addresses, so
 * kernel/mm/pmm.c contains no x86-specific knowledge.
 * Valid after arch_mm_init(). */
uint32_t                   arch_mm_reserved_region_count(void);
const aegis_mem_region_t  *arch_mm_get_reserved_regions(void);

/* Physical base address of the kernel image (arch-defined load address).
 * pmm_init() uses this to reserve the kernel image pages. */
#define ARCH_KERNEL_PHYS_BASE 0x100000UL

/* Virtual base address of the kernel in the higher half. */
#define ARCH_KERNEL_VIRT_BASE 0xFFFFFFFF80000000UL

/* -------------------------------------------------------------------------
 * Virtual memory interface (Phase 3+)
 * ------------------------------------------------------------------------- */

/* Load a physical PML4 page table address into CR3, replacing the current
 * address space. Called by vmm_init() after building the higher-half map. */
void arch_vmm_load_pml4(uint64_t phys);

/* Invalidate the TLB entry for a single virtual address.
 * Must be called after any PTE modification to ensure coherency. */
void arch_vmm_invlpg(uint64_t virt);

#endif
