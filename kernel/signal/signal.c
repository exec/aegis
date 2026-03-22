#include "signal.h"
#include "idt.h"
#include "proc.h"
#include "sched.h"
#include "syscall.h"
#include "uaccess.h"
#include "printk.h"
#include <stdint.h>

void
signal_deliver(cpu_state_t *s)
{
    (void)s;
    /* stub — Phase 17 Task 4 */
}

int
signal_deliver_sysret(syscall_frame_t *frame, uint64_t *saved_rdi_ptr)
{
    (void)frame; (void)saved_rdi_ptr;
    return 0;
}

void
signal_send_pid(uint32_t pid, int signum)
{
    (void)pid; (void)signum;
}

int
signal_check_pending(void)
{
    return 0;
}
