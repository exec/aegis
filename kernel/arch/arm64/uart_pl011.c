/*
 * uart_pl011.c — PL011 UART driver for QEMU virt machine.
 *
 * The QEMU virt machine places PL011 UART0 at physical address 0x09000000.
 * With MMU off (boot stage), we write directly to this physical address.
 *
 * PL011 register map (offsets from base):
 *   0x000  UARTDR   — data register (write: TX, read: RX)
 *   0x018  UARTFR   — flag register (bit 5 = TXFF = TX FIFO full)
 *
 * At reset, PL011 is enabled with 8N1 and a usable baud rate.
 * No initialization is needed for QEMU — just write to UARTDR.
 */

#include <stdint.h>

#define PL011_BASE  0x09000000UL
#define UARTDR      (*(volatile uint32_t *)(PL011_BASE + 0x000))
#define UARTFR      (*(volatile uint32_t *)(PL011_BASE + 0x018))
#define UARTFR_TXFF (1U << 5)

void
uart_putc(char c)
{
    /* Spin until TX FIFO has space */
    while (UARTFR & UARTFR_TXFF)
        ;
    UARTDR = (uint32_t)c;
}

void
uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

void
serial_write_string(const char *s)
{
    uart_puts(s);
}
