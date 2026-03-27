/* kernel/syscall/futex.c — futex WAIT/WAKE implementation */
#include "futex.h"
#include "sched.h"
#include "syscall_util.h"
#include "../mm/uaccess.h"

#define FUTEX_MAX_WAIT 64

typedef struct {
    aegis_task_t *task;
    uint64_t      addr;
    uint8_t       in_use;
} futex_waiter_t;

static futex_waiter_t s_pool[FUTEX_MAX_WAIT];

int futex_wake_addr(uint64_t addr, uint32_t count)
{
    int woken = 0;
    uint32_t i;
    for (i = 0; i < FUTEX_MAX_WAIT && (uint32_t)woken < count; i++) {
        if (s_pool[i].in_use && s_pool[i].addr == addr) {
            sched_wake(s_pool[i].task);
            s_pool[i].in_use = 0;
            s_pool[i].task = (aegis_task_t *)0;
            woken++;
        }
    }
    return woken;
}

uint64_t sys_futex(uint64_t addr, uint64_t op, uint64_t val,
                   uint64_t timeout, uint64_t addr2, uint64_t val3)
{
    (void)timeout; (void)addr2; (void)val3;
    uint32_t cmd = (uint32_t)op & ~(uint32_t)FUTEX_PRIVATE_FLAG;

    if (!user_ptr_valid(addr, sizeof(uint32_t)))
        return (uint64_t)-(int64_t)14; /* EFAULT */

    if (cmd == FUTEX_WAIT) {
        uint32_t uval;
        copy_from_user(&uval, (const void *)(uintptr_t)addr,
                       sizeof(uint32_t));
        if (uval != (uint32_t)val)
            return (uint64_t)-(int64_t)11; /* EAGAIN */
        /* Find free pool slot */
        uint32_t i;
        futex_waiter_t *w = (futex_waiter_t *)0;
        for (i = 0; i < FUTEX_MAX_WAIT; i++) {
            if (!s_pool[i].in_use) {
                w = &s_pool[i];
                break;
            }
        }
        if (!w)
            return (uint64_t)-(int64_t)12; /* ENOMEM */
        w->in_use = 1;
        w->task = sched_current();
        w->addr = addr;
        sched_block();
        /* Woken — slot freed by futex_wake_addr */
        return 0;
    }

    if (cmd == FUTEX_WAKE)
        return (uint64_t)futex_wake_addr(addr, (uint32_t)val);

    return (uint64_t)-(int64_t)38; /* ENOSYS */
}
