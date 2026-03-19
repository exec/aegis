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

#endif
