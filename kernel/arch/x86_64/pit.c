#include "pit.h"
#include "pic.h"
#include "arch.h"
#include "printk.h"
#include "random.h"
#include "../drivers/xhci.h"
#include "netdev.h"
#include "../../net/tcp.h"
#include "../../net/ip.h"

#define PIT_CHANNEL0 0x40
#define PIT_CMD      0x43
/* Channel 0, lobyte/hibyte, mode 3 (square wave) */
#define PIT_MODE     0x36
/* 1193182 Hz / 100 = 11931.82 → round to 11932 */
#define PIT_DIVISOR  11932

/* File-static: never accessed outside pit.c.
 * kernel/core/ uses arch_get_ticks() declared in arch.h. */
static volatile uint64_t s_ticks = 0;

/* Set by arch_request_shutdown(); checked in pit_handler each tick.
 * When set, pit_handler calls arch_debug_exit from within the ISR
 * (IF=0, no task context) to avoid the race where the task context
 * continues running after the port write and outputs a second line. */
static volatile int s_shutdown = 0;

/* Forward declaration: sched_tick is implemented in kernel/sched/sched.c.
 * We use a forward decl here to avoid a circular include dependency.
 * -Ikernel/sched is in CFLAGS so we could include sched.h, but the
 * forward decl is cleaner for a single-function dependency. */
void sched_tick(void);

void
pit_init(void)
{
    /* Program PIT channel 0 */
    outb(PIT_CMD, PIT_MODE);
    outb(PIT_CHANNEL0, PIT_DIVISOR & 0xFF);        /* low byte  */
    outb(PIT_CHANNEL0, (PIT_DIVISOR >> 8) & 0xFF); /* high byte */

    /* Unmask IRQ0 so the PIT starts firing */
    pic_unmask(0);

    printk("[PIT] OK: timer at 100 Hz\n");
}

void
pit_handler(void)
{
    s_ticks++;
    random_add_interrupt_entropy();
    sched_tick();
    xhci_poll();    /* poll USB event ring for HID reports (no-op if inactive) */
    netdev_poll_all();  /* poll registered network devices (virtio-net etc.) */
    ip_loopback_poll(); /* drain loopback queue (127.0.0.1, self-addressed) */
    tcp_tick();         /* Phase 25: TCP retransmit timer */
    /* Yield to QEMU's SLIRP event loop via port I/O (causes VM-exit on TCG).
     * The doorbell write in virtio_net_poll triggers virtio processing but not
     * SLIRP's connection accept loop.  A port I/O read forces QEMU's main event
     * loop to run select()/poll(), which processes SLIRP hostfwd connections.
     * Port 0x61 (PC speaker control) is safe to read in any state. */
    inb(0x61);
    /* Check shutdown AFTER sched_tick so the task that set s_shutdown
     * gets preempted cleanly before we call arch_debug_exit. */
    if (s_shutdown)
        arch_debug_exit(0x01);
}

/* arch_request_shutdown — called from task context to request a clean exit.
 * The actual arch_debug_exit is deferred to the next pit_handler invocation
 * (ISR context, IF=0) so no task code runs after the debug_exit port write. */
void
arch_request_shutdown(void)
{
    s_shutdown = 1;
}

/* arch_get_ticks — arch-boundary accessor for the tick counter.
 * Declared in arch.h so kernel/core/ can call it without including pit.h. */
uint64_t
arch_get_ticks(void)
{
    return s_ticks;
}
