#include "kbd.h"
#include "pic.h"
#include "arch.h"
#include "printk.h"
#include "signal.h"
#include "proc.h"
#include "sched.h"

#define KBD_DATA 0x60

/* 64-byte ring buffer */
#define KBD_BUF_SIZE 64
static volatile char    s_buf[KBD_BUF_SIZE];
static volatile uint32_t s_head = 0;  /* next write position */
static volatile uint32_t s_tail = 0;  /* next read position  */

/* Shift state */
static volatile int s_shift = 0;

/* Ctrl state and foreground process group for signal delivery */
static volatile int      s_ctrl     = 0;
static volatile uint32_t s_tty_pgrp = 0;

/* US QWERTY scancode set 1 — unshifted (make codes 0x01–0x39) */
static const char s_sc_lower[] = {
    0,    0,   '1', '2', '3', '4', '5', '6',  /* 0x00–0x07 */
    '7', '8', '9', '0', '-', '=',  '\b', '\t', /* 0x08–0x0F */
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',   /* 0x10–0x17 */
    'o', 'p', '[', ']', '\n',  0,  'a', 's',   /* 0x18–0x1F */
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',   /* 0x20–0x27 */
    '\'', '`', 0,  '\\','z', 'x', 'c', 'v',   /* 0x28–0x2F */
    'b', 'n', 'm', ',', '.', '/',  0,   '*',   /* 0x30–0x37 */
    0,   ' '                                    /* 0x38–0x39 */
};

/* US QWERTY scancode set 1 — shifted */
static const char s_sc_upper[] = {
    0,    0,   '!', '@', '#', '$', '%', '^',
    '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    'O', 'P', '{', '}', '\n',  0,  'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"', '~',  0,  '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?',  0,   '*',
    0,   ' '
};

#define SC_TABLE_SIZE ((int)(sizeof(s_sc_lower) / sizeof(s_sc_lower[0])))

static void
buf_push(char c)
{
    uint32_t next = (s_head + 1) & (KBD_BUF_SIZE - 1);
    if (next != s_tail) {   /* drop if full */
        s_buf[s_head] = c;
        s_head = next;
    }
}

void
kbd_init(void)
{
    pic_unmask(1);  /* IRQ1 = PS/2 keyboard */
    printk("[KBD] OK: PS/2 keyboard ready\n");
}

void
kbd_handler(void)
{
    uint8_t sc = inb(KBD_DATA);

    /* Extended key prefix — skip this byte (next byte is the actual key) */
    if (sc == 0xE0)
        return;

    /* Break code (key released) — bit 7 set */
    if (sc & 0x80) {
        uint8_t make = sc & 0x7F;
        /* Track shift releases */
        if (make == 0x2A || make == 0x36)
            s_shift = 0;
        if (make == 0x1D) { s_ctrl = 0; }
        return;
    }

    /* Make code */
    if (sc == 0x2A || sc == 0x36) {    /* left or right shift */
        s_shift = 1;
        return;
    }

    /* Ctrl key: left Ctrl = 0x1D make, 0x9D break */
    if (sc == 0x1D) { s_ctrl = 1; return; }

    /* Ctrl-C = Ctrl held + scancode 0x2E ('c') */
    if (s_ctrl && sc == 0x2E) {
        if (s_tty_pgrp != 0)
            signal_send_pgrp(s_tty_pgrp, SIGINT);
        return;
    }
    if (s_ctrl && sc == 0x2C) {
        if (s_tty_pgrp != 0)
            signal_send_pgrp(s_tty_pgrp, SIGTSTP);
        return;
    }
    if (s_ctrl && sc == 0x2B) {
        if (s_tty_pgrp != 0)
            signal_send_pgrp(s_tty_pgrp, SIGQUIT);
        return;
    }

    if (sc < SC_TABLE_SIZE) {
        char c = s_shift ? s_sc_upper[sc] : s_sc_lower[sc];
        if (c)
            buf_push(c);
    }
}

char
kbd_read(void)
{
    char c;
    /*
     * Re-enable interrupts while waiting for a keystroke.
     *
     * SYSCALL entry clears IF via SFMASK.  Without sti here, PS/2 IRQ1
     * is permanently held pending and kbd_handler never fires — keyboard
     * input is impossible.
     *
     * Pattern: sti → hlt (sleep until next IRQ) → cli when done.
     * hlt is atomic with respect to pending interrupts: if an IRQ arrives
     * between the poll and hlt, the CPU wakes immediately, so there is no
     * lost-wakeup race.  cli restores the IF=0 invariant for the remainder
     * of the syscall.
     */
    __asm__ volatile("sti");
    while (!kbd_poll(&c))
        __asm__ volatile("hlt");
    __asm__ volatile("cli");
    return c;
}

int
kbd_poll(char *out)
{
    if (s_head == s_tail)
        return 0;
    *out = s_buf[s_tail];
    s_tail = (s_tail + 1) & (KBD_BUF_SIZE - 1);
    return 1;
}

/* kbd_usb_inject — inject an ASCII character from USB HID into the keyboard
 * ring buffer.  Called from usb_hid_process_report() in interrupt context
 * (PIT ISR → xhci_poll → usb_hid_process_report → here).
 *
 * Shares the same ring buffer as the PS/2 kbd_handler so that USB and PS/2
 * keystrokes are delivered identically to kbd_read() / kbd_poll() callers.
 * No separate unblock needed: kbd_read() spins on hlt and will wake on the
 * next interrupt after buf_push() has placed the character. */
void
kbd_usb_inject(uint8_t ascii)
{
    if (ascii == 0)
        return;
    /* Intercept Ctrl-C (ETX=0x03), Ctrl-Z (SUB=0x1A), Ctrl-\ (FS=0x1C) */
    if (ascii == 0x03) {
        if (s_tty_pgrp != 0)
            signal_send_pgrp(s_tty_pgrp, SIGINT);
        return;
    }
    if (ascii == 0x1A) {
        if (s_tty_pgrp != 0)
            signal_send_pgrp(s_tty_pgrp, SIGTSTP);
        return;
    }
    if (ascii == 0x1C) {
        if (s_tty_pgrp != 0)
            signal_send_pgrp(s_tty_pgrp, SIGQUIT);
        return;
    }
    buf_push((char)ascii);
}

void
kbd_set_tty_pgrp(uint32_t pgid)
{
    s_tty_pgrp = pgid;
}

uint32_t
kbd_get_tty_pgrp(void)
{
    return s_tty_pgrp;
}

char
kbd_read_interruptible(int *interrupted)
{
    char c = 0;
    *interrupted = 0;
    __asm__ volatile("sti");
    for (;;) {
        if (kbd_poll(&c)) {
            __asm__ volatile("cli");
            return c;
        }
        /* Check for pending signals before halting — use canonical helper */
        if (signal_check_pending()) {
            __asm__ volatile("cli");
            *interrupted = 1;
            return '\0';
        }
        __asm__ volatile("hlt");
    }
}
