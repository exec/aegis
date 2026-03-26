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

/* Virtual base of the kernel in TTBR1 upper half.
 * VA = 0xFFFF000040000000; PA = 0x40000000.
 * Offset = VA - PA = 0xFFFF000000000000. */
#define ARCH_KERNEL_VIRT_BASE 0xFFFF000040000000UL
#define KERN_VA_OFFSET        0xFFFF000000000000UL

/* KVA bump allocator base — in TTBR1 range, past kernel + window. */
#define ARCH_KVA_BASE (ARCH_KERNEL_VIRT_BASE + 0xA00000UL)

/* -------------------------------------------------------------------------
 * Virtual memory interface
 * ------------------------------------------------------------------------- */

/* Load a page table root into TTBR0_EL1 and flush TLB. */
void arch_vmm_load_pml4(uint64_t phys);

/* Invalidate TLB entry for a single virtual address. */
void arch_vmm_invlpg(uint64_t virt);

/* -------------------------------------------------------------------------
 * Context switch + ring-3 support
 * ------------------------------------------------------------------------- */

struct aegis_task_t;
void ctx_switch(struct aegis_task_t *outgoing, struct aegis_task_t *incoming);

/* Set SP_EL1 (kernel stack for EL0→EL1 exceptions). */
void arch_set_kernel_stack(uint64_t sp0);

/* Set EL0 TLS base (TPIDR_EL0). */
static inline void
arch_set_fs_base(uint64_t addr)
{
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(addr));
}

/* Store master page table address for ISR/syscall CR3 restore.
 * On ARM64, TTBR1 handles kernel — this is a TTBR0 save for user restore. */
void arch_set_master_pml4(uint64_t pml4_phys);

/* Number of callee-saved register slots pushed by ctx_switch.
 * ARM64: x19-x28 (10) + x29/fp (1) + x30/lr (1) = 12 slots.
 * x30 (lr) serves as the return address — stored at [sp+0] after push.
 * sched_spawn must build a matching frame: 11 zeros + fn as lr. */
#define ARCH_CTX_SLOTS 12

/* -------------------------------------------------------------------------
 * Timer
 * ------------------------------------------------------------------------- */

/* Returns the tick count (incremented 100x/second by the generic timer). */
uint64_t arch_get_ticks(void);

/* Request a deferred shutdown (QEMU exit). */
void arch_request_shutdown(void);

/* -------------------------------------------------------------------------
 * PAN (Privileged Access Never) — ARM64 SMAP equivalent
 * ------------------------------------------------------------------------- */

/* Stubs — PAN not yet enabled. These match x86 arch_stac/arch_clac. */
extern int arch_smap_enabled;
static inline void arch_stac(void) { (void)0; }
static inline void arch_clac(void) { (void)0; }

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
