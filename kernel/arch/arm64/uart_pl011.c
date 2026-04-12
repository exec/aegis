/*
 * uart_pl011.c — PL011 UART driver.
 *
 * TX: uart_putc/uart_puts/serial_write_string
 * RX: uart_getc (blocking), uart_poll (non-blocking)
 * Ring buffer + GIC IRQ 33 (SPI 1) for interrupt-driven RX.
 *
 * The MMIO base is probed at boot: we try each of the known physical
 * addresses and read the PL011 peripheral ID at offset 0xFE0 (should
 * be 0x11 for a real PL011). The first hit wins.
 *
 *   QEMU virt:        PA 0x09000000
 *   BCM2712 (Pi 5):   PA 0x10_7D00_1000 — the internal debug UART
 *                     on the RP1-less MMIO bank. Pre-initialised by
 *                     the Pi firmware at 115200 8N1 from a 48 MHz
 *                     clock, so no re-init is needed here — we just
 *                     latch onto it. NOT 0xFE201000: that was the Pi
 *                     4 legacy peripheral window and does not exist
 *                     on BCM2712.
 *
 * The QEMU base is in the first 1 GiB device block that boot.S
 * populates (kern_l1[0]); the Pi 5 base sits inside kern_l1[4]'s
 * device block at 0x1_0000_0000..0x1_4000_0000.
 *
 * PL011 register map (offsets from base):
 *   0x000  UARTDR   — data register (write: TX, read: RX)
 *   0x018  UARTFR   — flag register (bit 4 = RXFE, bit 5 = TXFF)
 *   0x038  UARTIMSC — interrupt mask set/clear
 *   0x044  UARTICR  — interrupt clear register
 *   0xFE0  UARTPeriphID0 — reads 0x11 on PL011
 */

#include <stdint.h>

#define KERN_VA_OFFSET 0xFFFF000000000000UL

#define UART_OFF_DR       0x000
#define UART_OFF_FR       0x018
#define UART_OFF_IMSC     0x038
#define UART_OFF_ICR      0x044
#define UART_OFF_PERIPHID0 0xFE0

#define UARTFR_TXFF   (1U << 5)
#define UARTFR_RXFE   (1U << 4)  /* RX FIFO empty */

/* Resolved at arch_init time by uart_init(). */
static volatile uint32_t *s_uart_base;

static inline uint32_t
uart_r32(uint32_t off)
{
    return *(volatile uint32_t *)((uintptr_t)s_uart_base + off);
}
static inline void
uart_w32(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)((uintptr_t)s_uart_base + off) = val;
}

/* Called from arch_init before anything tries to print. Probes the
 * two known PL011 base addresses; falls back to the QEMU address if
 * neither responds (since we're writing to device memory either way,
 * a bad fallback just means no visible output — boot will hang
 * earlier than the kernel banner). */
#define PL011_QEMU_PA  0x09000000UL
#define PL011_PI5_PA   0x107D001000UL

void
uart_init(void)
{
    volatile uint32_t *qemu =
        (volatile uint32_t *)(PL011_QEMU_PA + KERN_VA_OFFSET);
    volatile uint32_t *pi5 =
        (volatile uint32_t *)(PL011_PI5_PA  + KERN_VA_OFFSET);

    if (qemu[UART_OFF_PERIPHID0 / 4] == 0x11) {
        s_uart_base = qemu;
    } else if (pi5[UART_OFF_PERIPHID0 / 4] == 0x11) {
        s_uart_base = pi5;
    } else {
        /* Neither probe matched — fall back to QEMU. If we're actually
         * on hardware this will just mean no visible output; the boot
         * will hang before the banner and the bug will be obvious. */
        s_uart_base = qemu;
    }
}

/* ── RX ring buffer ── */
#define UART_RX_SIZE 256
static char     s_rx_buf[UART_RX_SIZE];
static volatile uint32_t s_rx_head;  /* write index (ISR) */
static volatile uint32_t s_rx_tail;  /* read index (kbd_read) */

/* ── TX ── */

void
uart_putc(char c)
{
    while (uart_r32(UART_OFF_FR) & UARTFR_TXFF)
        ;
    uart_w32(UART_OFF_DR, (uint32_t)c);
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
    while (!(uart_r32(UART_OFF_FR) & UARTFR_RXFE)) {
        char c = (char)(uart_r32(UART_OFF_DR) & 0xFF);
        uint32_t next = (s_rx_head + 1) % UART_RX_SIZE;
        if (next != s_rx_tail) {  /* not full */
            s_rx_buf[s_rx_head] = c;
            s_rx_head = next;
        }
    }
    /* Clear RX interrupt */
    uart_w32(UART_OFF_ICR, 1U << 4);  /* RXIC */
}

/* Enable PL011 RX interrupt (called during init). */
void
uart_enable_rx_irq(void)
{
    /* Enable RX interrupt in PL011 */
    uint32_t imsc = uart_r32(UART_OFF_IMSC);
    uart_w32(UART_OFF_IMSC, imsc | (1U << 4));  /* RXIM */
}

/* Non-blocking read. Returns 1 if a char was available. */
int
uart_rx_poll(char *out)
{
    /* Also poll FIFO directly (in case IRQ hasn't fired yet) */
    while (!(uart_r32(UART_OFF_FR) & UARTFR_RXFE)) {
        char c = (char)(uart_r32(UART_OFF_DR) & 0xFF);
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
