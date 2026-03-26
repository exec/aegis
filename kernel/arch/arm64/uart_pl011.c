/*
 * uart_pl011.c — PL011 UART driver for QEMU virt machine.
 *
 * TX: uart_putc/uart_puts/serial_write_string
 * RX: uart_getc (blocking), uart_poll (non-blocking)
 * Ring buffer + GIC IRQ 33 (SPI 1) for interrupt-driven RX.
 *
 * PL011 register map (offsets from base):
 *   0x000  UARTDR   — data register (write: TX, read: RX)
 *   0x018  UARTFR   — flag register (bit 4 = RXFE, bit 5 = TXFF)
 *   0x038  UARTIMSC — interrupt mask set/clear
 *   0x044  UARTICR  — interrupt clear register
 */

#include <stdint.h>

#define PL011_BASE    0xFFFF000009000000UL
#define UARTDR        (*(volatile uint32_t *)(PL011_BASE + 0x000))
#define UARTFR        (*(volatile uint32_t *)(PL011_BASE + 0x018))
#define UARTIMSC      (*(volatile uint32_t *)(PL011_BASE + 0x038))
#define UARTICR       (*(volatile uint32_t *)(PL011_BASE + 0x044))
#define UARTFR_TXFF   (1U << 5)
#define UARTFR_RXFE   (1U << 4)  /* RX FIFO empty */

/* ── RX ring buffer ── */
#define UART_RX_SIZE 256
static char     s_rx_buf[UART_RX_SIZE];
static volatile uint32_t s_rx_head;  /* write index (ISR) */
static volatile uint32_t s_rx_tail;  /* read index (kbd_read) */

/* ── TX ── */

void
uart_putc(char c)
{
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

/* ── RX ── */

/* Called from GIC IRQ handler when PL011 fires (SPI 1 = IRQ 33). */
void
uart_rx_handler(void)
{
    while (!(UARTFR & UARTFR_RXFE)) {
        char c = (char)(UARTDR & 0xFF);
        uint32_t next = (s_rx_head + 1) % UART_RX_SIZE;
        if (next != s_rx_tail) {  /* not full */
            s_rx_buf[s_rx_head] = c;
            s_rx_head = next;
        }
    }
    /* Clear RX interrupt */
    UARTICR = (1U << 4);  /* RXIC */
}

/* Enable PL011 RX interrupt (called during init). */
void
uart_enable_rx_irq(void)
{
    /* Enable RX interrupt in PL011 */
    UARTIMSC |= (1U << 4);  /* RXIM */
}

/* Non-blocking read. Returns 1 if a char was available. */
int
uart_rx_poll(char *out)
{
    /* Also poll FIFO directly (in case IRQ hasn't fired yet) */
    while (!(UARTFR & UARTFR_RXFE)) {
        char c = (char)(UARTDR & 0xFF);
        uint32_t next = (s_rx_head + 1) % UART_RX_SIZE;
        if (next != s_rx_tail) {
            s_rx_buf[s_rx_head] = c;
            s_rx_head = next;
        }
    }

    if (s_rx_head == s_rx_tail)
        return 0;
    *out = s_rx_buf[s_rx_tail];
    s_rx_tail = (s_rx_tail + 1) % UART_RX_SIZE;
    return 1;
}

/* Blocking read. Spins until a character is available. */
char
uart_rx_read(void)
{
    char c;
    while (!uart_rx_poll(&c))
        __asm__ volatile("wfi");
    return c;
}
