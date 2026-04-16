#include "kbd.h"
#include "pic.h"
#include "arch.h"
#include "printk.h"
#include "random.h"
#include "signal.h"
#include "tty.h"
#include "sched.h"
#include "spinlock.h"
#include "../../sched/waitq.h"

extern waitq_t g_console_waiters;

#define KBD_DATA 0x60

/* 64-byte ring buffer */
#define KBD_BUF_SIZE 64
static volatile char    s_buf[KBD_BUF_SIZE];
static volatile uint32_t s_head = 0;  /* next write position */
static volatile uint32_t s_tail = 0;  /* next read position  */

static spinlock_t kbd_lock = SPINLOCK_INIT;

/* Task blocked on kbd_read_interruptible — woken by buf_push */
static aegis_task_t *s_kbd_waiter = 0;

/* Shift state */
static volatile int s_shift = 0;

/* Ctrl state */
static volatile int s_ctrl = 0;

/* Alt state */
static volatile int s_alt = 0;

/* US QWERTY scancode set 1 — unshifted (make codes 0x01–0x39) */
static const char s_sc_lower[] = {
    0,    0,   '1', '2', '3', '4', '5', '6',  /* 0x00–0x07 */
    '7', '8', '9', '0', '-', '=',  127,  '\t', /* 0x08–0x0F */
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',   /* 0x10–0x17 */
    'o', 'p', '[', ']', '\r',  0,  'a', 's',   /* 0x18–0x1F */
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',   /* 0x20–0x27 */
    '\'', '`', 0,  '\\','z', 'x', 'c', 'v',   /* 0x28–0x2F */
    'b', 'n', 'm', ',', '.', '/',  0,   '*',   /* 0x30–0x37 */
    0,   ' '                                    /* 0x38–0x39 */
};

/* US QWERTY scancode set 1 — shifted */
static const char s_sc_upper[] = {
    0,    0,   '!', '@', '#', '$', '%', '^',
    '&', '*', '(', ')', '_', '+', 127,  '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    'O', 'P', '{', '}', '\r',  0,  'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"', '~',  0,  '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?',  0,   '*',
    0,   ' '
};

#define SC_TABLE_SIZE ((int)(sizeof(s_sc_lower) / sizeof(s_sc_lower[0])))

static void
buf_push(char c)
{
    irqflags_t fl = spin_lock_irqsave(&kbd_lock);
    uint32_t next = (s_head + 1) & (KBD_BUF_SIZE - 1);
    if (next != s_tail) {   /* drop if full */
        s_buf[s_head] = c;
        s_head = next;
    }
    /* Wake any task blocked in kbd_read_interruptible */
    if (s_kbd_waiter) {
        sched_wake(s_kbd_waiter);
        s_kbd_waiter = 0;
    }
    spin_unlock_irqrestore(&kbd_lock, fl);
    /* Wake any pollers on /dev/tty or /dev/console waiting for input.
     * waitq_wake_all is documented ISR-safe. Done outside kbd_lock to
     * avoid holding nested locks across the wake. */
    waitq_wake_all(&g_console_waiters);
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
    random_add_interrupt_entropy();  /* keyboard timing is excellent entropy */

    /* Extended key prefix — set flag, handle next byte */
    static int s_e0_prefix;
    if (sc == 0xE0) {
        s_e0_prefix = 1;
        return;
    }

    /* E0-prefixed keys: arrow keys → ESC [ A/B/C/D sequences */
    if (s_e0_prefix) {
        s_e0_prefix = 0;
        if (sc & 0x80) {
            /* E0 break code — track modifier releases */
            uint8_t make = sc & 0x7F;
            if (make == 0x1D) s_ctrl = 0;  /* right Ctrl release */
            return;
        }
        /* E0 make codes */
        if (sc == 0x1D) { s_ctrl = 1; return; }  /* right Ctrl */
        char arrow = 0;
        switch (sc) {
        case 0x48: arrow = 'A'; break;  /* up */
        case 0x50: arrow = 'B'; break;  /* down */
        case 0x4B: arrow = 'D'; break;  /* left */
        case 0x4D: arrow = 'C'; break;  /* right */
        case 0x47: arrow = 'H'; break;  /* Home */
        case 0x4F: arrow = 'F'; break;  /* End */
        }
        if (arrow) {
            buf_push('\033');
            buf_push('[');
            buf_push(arrow);
        }
        return;
    }

    /* Break code (key released) — bit 7 set */
    if (sc & 0x80) {
        uint8_t make = sc & 0x7F;
        /* Track shift releases */
        if (make == 0x2A || make == 0x36)
            s_shift = 0;
        if (make == 0x1D) { s_ctrl = 0; }
        if (make == 0x38) { s_alt = 0; }
        return;
    }

    /* Make code */
    if (sc == 0x2A || sc == 0x36) {    /* left or right shift */
        s_shift = 1;
        return;
    }

    /* Ctrl key: left Ctrl = 0x1D make, 0x9D break */
    if (sc == 0x1D) { s_ctrl = 1; return; }

    /* Alt key: left Alt = 0x38 make, 0xB8 break */
    if (sc == 0x38) { s_alt = 1; return; }

    /* Ctrl-D = EOF: push 0x04 (EOT) into ring buffer for line discipline */
    if (s_ctrl && sc == 0x20) {
        buf_push(0x04);
        return;
    }

    if (sc < SC_TABLE_SIZE) {
        char c = s_shift ? s_sc_upper[sc] : s_sc_lower[sc];
        if (c) {
            if (s_ctrl) c &= 0x1F;  /* General Ctrl: mask to control char */
            /* Ctrl+Shift: ESC prefix so compositor can distinguish from plain Ctrl.
             * e.g., Ctrl+Shift+C = ESC + 0x03, plain Ctrl+C = 0x03 */
            if (s_ctrl && s_shift) buf_push(0x1B);
            if (s_alt) buf_push(0x1B);  /* Alt: ESC prefix */
            buf_push(c);
        }
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
    for (;;) {
        s_kbd_waiter = sched_current();
        if (kbd_poll(&c)) {
            s_kbd_waiter = 0;
            return c;
        }
        sched_block();
    }
}

int
kbd_poll(char *out)
{
    irqflags_t fl = spin_lock_irqsave(&kbd_lock);
    if (s_head == s_tail) {
        spin_unlock_irqrestore(&kbd_lock, fl);
        return 0;
    }
    *out = s_buf[s_tail];
    s_tail = (s_tail + 1) & (KBD_BUF_SIZE - 1);
    spin_unlock_irqrestore(&kbd_lock, fl);
    return 1;
}

int
kbd_has_data(void)
{
    irqflags_t fl = spin_lock_irqsave(&kbd_lock);
    int has = (s_head != s_tail);
    spin_unlock_irqrestore(&kbd_lock, fl);
    return has;
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
    buf_push((char)ascii);
}

/* Deferred foreground pgrp — set before console tty is initialized.
 * Applied to tty_console()->fg_pgrp once the console tty exists. */
static volatile uint32_t s_deferred_pgrp = 0;

void
kbd_set_tty_pgrp(uint32_t pgid)
{
    tty_t *con = tty_console();
    if (con)
        con->fg_pgrp = pgid;
    else
        s_deferred_pgrp = pgid;
}

uint32_t
kbd_get_tty_pgrp(void)
{
    tty_t *con = tty_console();
    return con ? con->fg_pgrp : s_deferred_pgrp;
}

char
kbd_read_interruptible(int *interrupted)
{
    char c = 0;
    *interrupted = 0;
    for (;;) {
        s_kbd_waiter = sched_current();
        if (kbd_poll(&c)) {
            s_kbd_waiter = 0;
            return c;
        }
        if (signal_check_pending()) {
            s_kbd_waiter = 0;
            *interrupted = 1;
            return '\0';
        }
        /* Block until kbd ISR pushes a character and wakes us */
        sched_block();
    }
}
