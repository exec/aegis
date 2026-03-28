/* usb_mouse.c — USB HID boot-protocol mouse driver
 *
 * Parses 3-byte boot protocol mouse reports into mouse_event_t structs
 * and stores them in a ring buffer. /dev/mouse reads from this buffer.
 *
 * Ring buffer: 128 entries (~900 bytes BSS). Static allocation.
 * Blocking read uses sti/hlt/cli pattern (same as kbd_read).
 */
#include "usb_mouse.h"
#include "../sched/sched.h"
#include <stddef.h>

#define MOUSE_BUF_SIZE 128

static mouse_event_t s_buf[MOUSE_BUF_SIZE];
static volatile uint32_t s_head = 0;
static volatile uint32_t s_tail = 0;

/* s_waiter — task blocked in mouse_read_blocking(), or NULL.
 * Set before sched_block(); cleared on wake. */
static aegis_task_t *s_waiter = NULL;

static void
buf_push(const mouse_event_t *evt)
{
    uint32_t next = (s_head + 1) % MOUSE_BUF_SIZE;
    if (next != s_tail) {
        s_buf[s_head] = *evt;
        s_head = next;
    }
    /* Wake blocked reader if any */
    if (s_waiter) {
        sched_wake(s_waiter);
        s_waiter = NULL;
    }
}

void
usb_mouse_process_report(const uint8_t *data, uint32_t len)
{
    if (len < 3) return;

    mouse_event_t evt;
    evt.buttons = data[0];
    evt.dx      = (int16_t)(int8_t)data[1];
    evt.dy      = (int16_t)(int8_t)data[2];
    evt.scroll  = 0;

    buf_push(&evt);
}

int
mouse_poll(mouse_event_t *out)
{
    if (s_head == s_tail)
        return 0;
    *out = s_buf[s_tail];
    s_tail = (s_tail + 1) % MOUSE_BUF_SIZE;
    return 1;
}

void
mouse_inject(uint8_t buttons, int16_t dx, int16_t dy)
{
    mouse_event_t evt;
    evt.buttons = buttons;
    evt.dx      = dx;
    evt.dy      = dy;
    evt.scroll  = 0;
    buf_push(&evt);
}

void
mouse_read_blocking(mouse_event_t *out)
{
    __asm__ volatile("sti");
    while (!mouse_poll(out)) {
        s_waiter = sched_current();
        sched_block();
        /* Resumes here after sched_wake() from buf_push() */
    }
    __asm__ volatile("cli");
}
