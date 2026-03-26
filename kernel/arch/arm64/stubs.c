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

/* arch_debug_exit — write to QEMU virt PSCI / semihosting to exit.
 * For now, just loop. */
void
arch_debug_exit(unsigned char value)
{
    (void)value;
    for (;;)
        __asm__ volatile("wfi");
}
