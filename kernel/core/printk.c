#include "arch.h"
#include "printk.h"

/*
 * printk — route a string to all available output sinks.
 *
 * Law 1: serial is written unconditionally (if serial_init was called).
 * Law 2: VGA is written only if vga_available is set by vga_init().
 *        VGA failure never silences serial. Serial failure = kernel is blind.
 *
 * No format string support in Phase 1. Add when needed.
 */
void printk(const char *s)
{
    serial_write_string(s);
    if (vga_available) {
        vga_write_string(s);
    }
}
