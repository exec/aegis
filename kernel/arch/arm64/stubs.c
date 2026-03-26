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

void
vga_write_string(const char *s)
{
    (void)s;
}

/* Framebuffer is not available at this stage. */
int fb_available = 0;

void
fb_write_string(const char *s)
{
    (void)s;
}

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

/* ── Stubs for subsystems not yet ported ──────────────────────────── */

/* sched.c calls these — stub until user process support is added */
void arch_set_kernel_stack(uint64_t sp0) { (void)sp0; }
void arch_set_master_pml4(uint64_t pml4_phys) { (void)pml4_phys; }

/* sched.c exit path calls ext2_sync and signal_send_pid */
void ext2_sync(void) {}
void signal_send_pid(uint32_t pid, int sig) { (void)pid; (void)sig; }

/* proc.h referenced from sched.c — needs proc_spawn_init stub */
void proc_spawn_init(void) {}
