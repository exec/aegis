#include "kbd.h"
#include "pic.h"
#include "arch.h"
#include "printk.h"

#define KBD_DATA 0x60

/* 64-byte ring buffer */
#define KBD_BUF_SIZE 64
static volatile char    s_buf[KBD_BUF_SIZE];
static volatile uint32_t s_head = 0;  /* next write position */
static volatile uint32_t s_tail = 0;  /* next read position  */

/* Shift state */
static volatile int s_shift = 0;

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
        return;
    }

    /* Make code */
    if (sc == 0x2A || sc == 0x36) {    /* left or right shift */
        s_shift = 1;
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
    while (!kbd_poll(&c))
        ;   /* spin — task_kbd yields on next timer tick */
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
