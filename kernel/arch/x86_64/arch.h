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
 * Arch-specific port I/O primitives
 * Used by serial, PIC, PIT, VGA hardware cursor — not for use in kernel/core/.
 * ------------------------------------------------------------------------- */
static inline void
outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t
inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

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

/* -------------------------------------------------------------------------
 * Context switch (Phase 4+)
 * ------------------------------------------------------------------------- */

/* Forward declaration for the scheduler's TCB type.
 * sched.h defines the full struct; arch.h only needs the pointer. */
struct aegis_task_t;

/* Save outgoing task's callee-saved registers and RSP; restore incoming task's.
 * Implemented in kernel/arch/x86_64/ctx_switch.asm.
 *   outgoing — pointer to the current task's aegis_task_t (rsp field saved here)
 *   incoming — pointer to the next task's aegis_task_t   (rsp field loaded from here)
 * Never returns to the caller in the outgoing task until ctx_switch is called
 * again with that task as the incoming argument. */
void ctx_switch(struct aegis_task_t *outgoing, struct aegis_task_t *incoming);

/* -------------------------------------------------------------------------
 * Phase 4: Interrupt infrastructure
 * ------------------------------------------------------------------------- */

/* IDT: install 48 interrupt gate descriptors and load with lidt. */
void idt_init(void);

/* PIC: remap 8259A dual PIC so IRQ0-15 land at vectors 0x20-0x2F.
 * Masks all IRQs after init; drivers call pic_unmask(irq) when ready. */
void pic_init(void);

/* PIT: program channel 0 at 100 Hz and unmask IRQ0. */
void pit_init(void);

/* Returns the current PIT tick count (incremented 100x/second).
 * Use this instead of accessing the pit.c-internal counter directly. */
uint64_t arch_get_ticks(void);

/* -------------------------------------------------------------------------
 * Phase 4: Keyboard
 * ------------------------------------------------------------------------- */

/* Initialize PS/2 keyboard and unmask IRQ1. */
void kbd_init(void);

/* Blocking read — spins until a keypress is available. */
char kbd_read(void);

/* Non-blocking read. Returns 1 if a char was available and written to *out. */
int kbd_poll(char *out);

#endif
