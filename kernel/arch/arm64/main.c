/*
 * main.c — ARM64 kernel entry point (minimal boot stage).
 *
 * Called from boot.S with MMU off. DTB physical address in x0.
 * Goal: prove the toolchain, boot path, and serial output work.
 */

#include <stdint.h>

/* Provided by uart_pl011.c */
void uart_puts(const char *s);

void
kernel_main(uint64_t dtb_phys)
{
    (void)dtb_phys;

    uart_puts("[SERIAL] OK: PL011 UART initialized\n");
    uart_puts("[AEGIS] ARM64 boot stub reached.\n");
    uart_puts("[AEGIS] System halted.\n");

    /* Halt */
    for (;;)
        __asm__ volatile("wfi");
}
