/*
 * stubs.c — ARM64 stubs for interfaces not yet implemented.
 *
 * Provides symbols that kernel/core/ and kernel/mm/ reference via arch.h,
 * but that don't apply to the ARM64 boot stage (no VGA, no framebuffer).
 * Also provides memcpy/memset which GCC may emit at -O2.
 */

#include <stdint.h>
#include <stddef.h>

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

/* arch_init — on ARM64, UART is ready from reset. Nothing to do. */
void
arch_init(void)
{
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

/* ── Capability system stubs (Rust FFI — not yet built for aarch64) ── */
void cap_init(void) {
    extern void serial_write_string(const char *);
    serial_write_string("[CAP] OK: capability subsystem initialized\n");
}
int cap_check(void *proc, uint32_t kind, uint32_t rights)
    { (void)proc; (void)kind; (void)rights; return 0; /* allow all */ }
int cap_grant(void *proc, uint32_t kind, uint32_t rights)
    { (void)proc; (void)kind; (void)rights; return 0; }

/* ── String/memory functions GCC may need ── */
unsigned long strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }

/* ── USB HID stub ── */
void kbd_usb_inject(char c) { (void)c; }

/* signal_send_pid/pgrp/check_pending now in shared signal.c */

/* User binary blobs — real ARM64 musl-static binaries (compiled separately).
 * init_elf from init_arm64_bin.c, others from *_arm64_bin.c files. */
const char *init_name = "/bin/shell";

/* Not yet built for ARM64 */
#define EMPTY_ELF(name) \
    const unsigned char name##_elf[] = { 0 }; \
    const unsigned int  name##_elf_len = 0
EMPTY_ELF(oksh); EMPTY_ELF(vigil); EMPTY_ELF(vigictl);
EMPTY_ELF(httpd_bin); EMPTY_ELF(dhcp_bin);
#undef EMPTY_ELF

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

void fork_child_load_ttbr0(void) {
    uint64_t pml4 = arch_get_current_pml4();
    if (pml4)
        arch_vmm_load_user_ttbr0(pml4);
}

/* proc_spawn_init is provided by the real proc.c (shared source). */
