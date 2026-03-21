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

/* Request a clean shutdown. Sets a flag checked by pit_handler on the next
 * timer tick; actual arch_debug_exit is deferred to ISR context (IF=0) to
 * prevent the QEMU async isa-debug-exit race where task code re-runs after
 * the port write. */
void arch_request_shutdown(void);

/* -------------------------------------------------------------------------
 * Phase 4: Keyboard
 * ------------------------------------------------------------------------- */

/* Initialize PS/2 keyboard and unmask IRQ1. */
void kbd_init(void);

/* Blocking read — spins until a keypress is available. */
char kbd_read(void);

/* Non-blocking read. Returns 1 if a char was available and written to *out. */
int kbd_poll(char *out);

/* -------------------------------------------------------------------------
 * Phase 5: Ring-3 support
 * ------------------------------------------------------------------------- */

/* Build a 7-entry runtime GDT (null, kernel code/data, user data/code, TSS),
 * install with lgdt, reload segment registers, load TSS with ltr.
 * Prints [GDT] OK. */
void arch_gdt_init(void);

/* Set TSS.iomap_base = 104 (disables I/O permission bitmap).
 * Prints [TSS] OK. */
void arch_tss_init(void);

/* Update both TSS.RSP0 and g_kernel_rsp to rsp0.
 * Called by scheduler before every ctx_switch so:
 *   — CPU loads correct kernel stack top on ring-3 interrupts (via TSS.RSP0)
 *   — syscall_entry.asm loads correct kernel stack (via g_kernel_rsp)
 * Both values must always be identical. */
void arch_set_kernel_stack(uint64_t rsp0);

/* Store the master (kernel) PML4 physical address in g_master_pml4.
 * Must be called once after vmm_init(). isr.asm and syscall_entry.asm
 * load g_master_pml4 into CR3 at the start of every interrupt/syscall
 * so that kernel code always runs with the master PML4 (where TCBs and
 * kernel stacks are identity-mapped). */
void arch_set_master_pml4(uint64_t pml4_phys);

/* Program IA32_EFER (SCE), IA32_STAR, IA32_LSTAR, IA32_SFMASK.
 * Prints [SYSCALL] OK. */
void arch_syscall_init(void);

/* -------------------------------------------------------------------------
 * Phase 8: SMAP (Supervisor Mode Access Prevention)
 * ------------------------------------------------------------------------- */

/* arch_smap_init — detect SMAP via CPUID and enable CR4.SMAP if supported.
 * Prints [SMAP] OK: supervisor access prevention active, or
 * [SMAP] WARN: not supported by CPU. Must be called after arch_syscall_init(). */
void arch_smap_init(void);

/* arch_sse_init — enable SSE/SSE2 for user-mode execution.
 * Clears CR0.EM, sets CR0.MP, sets CR4.OSFXSR + CR4.OSXMMEXCPT.
 * Must be called before sched_start() so that user processes can execute
 * SSE instructions without triggering #UD (exception 6).
 * The kernel itself is compiled -mno-sse and never touches XMM registers. */
void arch_sse_init(void);

/* Set to 1 by arch_smap_init() after successfully enabling CR4.SMAP.
 * Checked by arch_stac/arch_clac to avoid #UD on CPUs without SMAP. */
extern int arch_smap_enabled;

/* arch_stac — set RFLAGS.AC, temporarily permitting ring-0 access to
 * user-mode pages under SMAP. Bracket ONLY the single instruction that
 * loads from a user address; always pair with arch_clac() immediately after.
 * Never call any function between arch_stac() and arch_clac().
 * The "memory" clobber prevents the compiler from hoisting user-memory
 * loads before stac. No-op if SMAP is not enabled (guards against #UD). */
static inline void arch_stac(void) { if (arch_smap_enabled) __asm__ volatile("stac" ::: "memory"); }

/* arch_clac — clear RFLAGS.AC, re-enabling SMAP protection.
 * Must be called after every arch_stac(). The "memory" clobber prevents
 * the compiler from sinking user-memory loads past clac.
 * No-op if SMAP is not enabled (guards against #UD). */
static inline void arch_clac(void) { if (arch_smap_enabled) __asm__ volatile("clac" ::: "memory"); }

/* -------------------------------------------------------------------------
 * Phase 14: FS segment base (TLS)
 * ------------------------------------------------------------------------- */

/* arch_set_fs_base — write addr to IA32_FS_BASE MSR (0xC0000100).
 * Used by sys_arch_prctl(ARCH_SET_FS) to configure musl's TLS pointer.
 * The SYSCALL instruction does not save/restore FS.base, so the value
 * set here persists across all subsequent syscalls for the lifetime of
 * the process. When a second user process is introduced, ctx_switch must
 * save proc->fs_base on outgoing and call arch_set_fs_base(proc->fs_base)
 * on incoming before returning to user space. */
static inline void
arch_set_fs_base(uint64_t addr)
{
    uint32_t lo = (uint32_t)addr;
    uint32_t hi = (uint32_t)(addr >> 32);
    __asm__ volatile ("wrmsr" : : "c"(0xC0000100U), "a"(lo), "d"(hi));
}

#endif
