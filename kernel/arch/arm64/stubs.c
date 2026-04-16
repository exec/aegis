/*
 * stubs.c — ARM64 stubs for interfaces not yet implemented.
 *
 * Provides symbols that kernel/core/ and kernel/mm/ reference via arch.h,
 * but that don't apply to the ARM64 boot stage (no VGA, no framebuffer).
 * Also provides memcpy/memset which GCC may emit at -O2.
 */

#include <stdint.h>
#include <stddef.h>
#include "arch.h"  /* for arch_set_fs_base */
#include "printk.h"
#include "smp.h"

/* ── Per-CPU storage ──────────────────────────────────────────────────
 * percpu_t g_percpu[MAX_CPUS] is the per-CPU data area. TPIDR_EL1 on each
 * CPU points at its slot. On ARM64 we currently run single-core so only
 * g_percpu[0] is used. Must be initialized before any sched_current()
 * call — percpu_current() reads the pointer out of TPIDR_EL1 and will
 * dereference garbage otherwise. */
percpu_t g_percpu[MAX_CPUS];
uint32_t g_cpu_count = 1;
volatile uint8_t g_ap_online[MAX_CPUS];

void
smp_percpu_init_bsp(void)
{
    percpu_t *p = &g_percpu[0];
    p->self = p;
    p->cpu_id = 0;
    p->affinity_id = 0;
    p->current_task = (struct aegis_task_t *)0;
    p->kernel_stack = 0;
    p->user_sp_scratch = 0;
    p->ticks = 0;
    p->prev_dying_tcb = (void *)0;
    p->prev_dying_stack = (void *)0;
    p->prev_dying_stack_pages = 0;
    __asm__ volatile("msr tpidr_el1, %0" : : "r"(p));
    __asm__ volatile("isb");
}

/* GCC emits calls to memcpy/memset even with -ffreestanding.
 * Provide minimal implementations. */
void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

/* VGA is not available on ARM64 QEMU virt. */
int vga_available = 0;

/* PAN not yet enabled. */
int arch_smap_enabled = 0;

void
vga_write_string(const char *s)
{
    (void)s;
}

/* fb_available and fb_write_string are provided by the real fb.c
 * (compiled as a shared source). fb.c handles the fb_available=0 case. */

/* From uart_pl011.c — probes QEMU vs Pi PL011 base at boot. */
void uart_init(void);

/* arch_init — runs before anything prints. Resolve the PL011 MMIO
 * base (QEMU virt vs Pi) so subsequent printks work on either board. */
void
arch_init(void)
{
    uart_init();
}

/* arch_debug_exit — use QEMU semihosting to exit.
 * QEMU virt with -semihosting supports the angel SYS_EXIT call. */
void
arch_debug_exit(unsigned char value)
{
    (void)value;
    for (;;)
        __asm__ volatile("wfi");
}

void
arch_request_shutdown(void)
{
    arch_debug_exit(1);
}

/* ── Capability system ─────────────────────────────────────────────
 * cap_init / cap_grant / cap_check are provided by the real Rust crate
 * in kernel/cap/ (built for aarch64-unknown-none and linked as libcap.a
 * via kernel/arch/arm64/Makefile). The previous allow-all stubs had the
 * wrong signature (3-arg vs cap.h's 4-arg), silently returning 0 for
 * every check, so cap enforcement was not just broken — it was fake. */

/* ── String/memory functions GCC may need ── */
unsigned long strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }

/* ── USB HID stub ── */
void kbd_usb_inject(char c) { (void)c; }

/* signal_send_pid/pgrp/check_pending now in shared signal.c */

/* Binary blobs (vigil, stsh, coreutils) are now produced by the
 * objcopy-based blob pipeline in kernel/arch/arm64/Makefile and
 * exposed as _binary_<name>_bin_{start,end} just like x86. The old
 * empty-blob fallbacks that lived here have been retired. */

/* proc_enter_user and fork_child_return are now in proc_enter.S */

/* ── Stubs for subsystems not yet ported ──────────────────────────── */

/* kbd implementation — backed by PL011 UART RX on ARM64.
 * uart_rx_read/poll/enable_rx_irq are in uart_pl011.c. */
extern char uart_rx_read(void);
extern int  uart_rx_poll(char *out);
extern void uart_enable_rx_irq(void);
extern void gic_enable_irq(uint32_t irq);

void kbd_init(void) {
    uart_enable_rx_irq();
    gic_enable_irq(33);  /* PL011 UART0 = SPI 1 = GIC IRQ 33 */
}
void kbd_handler(void) {}
char kbd_read(void) { return uart_rx_read(); }
char kbd_read_interruptible(int *interrupted) {
    /* TODO: check signal pending for interrupt support */
    (void)interrupted;
    return uart_rx_read();
}
int kbd_poll(char *out) { return uart_rx_poll(out); }
int kbd_has_data(void) {
    /* Non-destructive check not yet implemented for PL011 RX. Always 0. */
    return 0;
}
static uint32_t s_tty_pgrp = 0;
void     kbd_set_tty_pgrp(uint32_t pgid) { s_tty_pgrp = pgid; }
uint32_t kbd_get_tty_pgrp(void) { return s_tty_pgrp; }

/* sched.c calls these — stub until user process support is added */
void arch_set_kernel_stack(uint64_t sp0) {
    /* On ARM64 with SPSel=1, the kernel uses SP (= SP_EL1 at EL1).
     * Exceptions from EL0 auto-use SP_EL1. The scheduler's ctx_switch
     * already switches SP to the incoming task's kernel stack. */
    (void)sp0;
}
uint64_t g_master_pml4 = 0;
void arch_set_master_pml4(uint64_t pml4_phys) { g_master_pml4 = pml4_phys; }

/* ext2_sync provided by real ext2_cache.c.
 * signal_send_pid/pgrp provided by shared signal.c. */

/* Called from fork_child_return in proc_enter.S to load the child's TTBR0. */
extern void arch_vmm_load_user_ttbr0(uint64_t phys);
extern uint64_t arch_get_current_pml4(void);  /* in proc.c */

/* Save TPIDR_EL0 to proc->fs_base on every EL0 exception entry.
 * musl sets TPIDR_EL0 directly in user space; the kernel captures it here. */
extern void arch_save_current_fs_base(uint64_t val);  /* in proc.c */

void save_user_tpidr(void) {
    uint64_t tpidr;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tpidr));
    if (tpidr)
        arch_save_current_fs_base(tpidr);
}

/* Called from fork_child_return and EL0 IRQ return to set TTBR0 + TLS. */
extern uint64_t arch_get_current_fs_base(void);  /* in proc.c */

void fork_child_load_ttbr0(void) {
    uint64_t pml4 = arch_get_current_pml4();
    if (pml4)
        arch_vmm_load_user_ttbr0(pml4);
    /* Restore TLS for the current task. On ARM64, TPIDR_EL0 is set by
     * musl in userspace and saved to proc->fs_base on every EL0 exception
     * entry (see save_user_tpidr below). */
    uint64_t fs = arch_get_current_fs_base();
    if (fs)
        arch_set_fs_base(fs);
}

/* proc_spawn_init is provided by the real proc.c (shared source).
 * On ARM64, proc.c uses init_elf[] + init_elf_len from init_arm64_bin.c
 * instead of the x86 objcopy blob symbols. */

/* g_timer_waitq is defined in kernel/sched/waitq.c (cross-arch).
 * On ARM64 there is no PIT yet, so nothing wakes it — sys_poll/
 * sys_epoll_wait fall through to fd-readiness wakes once Tasks
 * 4-10 wire those up. */
