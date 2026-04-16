/* usb_mouse.c — USB HID boot-protocol mouse driver
 *
 * Parses 3-byte boot protocol mouse reports into mouse_event_t structs
 * and stores them in a ring buffer. /dev/mouse reads from this buffer.
 *
 * Ring buffer: 128 entries (~900 bytes BSS). Static allocation.
 * Blocking read uses sti/hlt/cli pattern (same as kbd_read).
 */
#include "usb_mouse.h"
#include "sched.h"
#include "spinlock.h"
#include "arch.h"
#include "../sched/waitq.h"
#include <stddef.h>

#define MOUSE_BUF_SIZE 128

static mouse_event_t s_buf[MOUSE_BUF_SIZE];
static volatile uint32_t s_head = 0;
static volatile uint32_t s_tail = 0;

static spinlock_t mouse_lock = SPINLOCK_INIT;

/* s_waiter — task blocked in mouse_read_blocking(), or NULL.
 * Set before sched_block(); cleared on wake. */
static aegis_task_t *s_waiter = NULL;

/* g_mouse_waiters — non-static so the /dev/mouse VFS ops in initrd.c can
 * extern it for get_waitq. Woken from buf_push() in HID dispatch context
 * (PIT ISR → xhci_poll → usb_hid_process_report → buf_push). */
waitq_t g_mouse_waiters = WAITQ_INIT;

static void
buf_push(const mouse_event_t *evt)
{
    irqflags_t fl = spin_lock_irqsave(&mouse_lock);
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
    spin_unlock_irqrestore(&mouse_lock, fl);
    /* Wake any pollers on /dev/mouse. waitq_wake_all is documented
     * ISR-safe; called outside mouse_lock to avoid nested locks. */
    waitq_wake_all(&g_mouse_waiters);
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
    irqflags_t fl = spin_lock_irqsave(&mouse_lock);
    if (s_head == s_tail) {
        spin_unlock_irqrestore(&mouse_lock, fl);
        return 0;
    }
    *out = s_buf[s_tail];
    s_tail = (s_tail + 1) % MOUSE_BUF_SIZE;
    spin_unlock_irqrestore(&mouse_lock, fl);
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
    arch_enable_irq();
    for (;;) {
        s_waiter = sched_current();
        if (mouse_poll(out)) {
            s_waiter = NULL;
            break;
        }
        sched_block();
        /* Resumes here after sched_wake() from buf_push() */
    }
    arch_disable_irq();
}
