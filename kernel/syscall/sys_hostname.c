/* sys_hostname.c — kernel-managed hostname (sethostname/uname source) */
#include "sys_impl.h"
#include "sched.h"
#include "proc.h"
#include "spinlock.h"
#include "cap.h"

/* HOSTNAME_MAX matches the utsname.nodename field (65 bytes incl. NUL).
 * Linux defines HOST_NAME_MAX as 64; the trailing byte is reserved for NUL. */
#define HOSTNAME_MAX 64

static spinlock_t s_hostname_lock = SPINLOCK_INIT;
static char       s_hostname[HOSTNAME_MAX + 1] = "aegis";
static uint32_t   s_hostname_len = 5;

/*
 * hostname_get — copy current hostname into out (NUL-terminated).
 * out must point to at least n bytes; if n is too small the result is
 * truncated and still NUL-terminated.  Safe to call from any context.
 */
void
hostname_get(char *out, uint32_t n)
{
    if (n == 0) return;
    irqflags_t fl = spin_lock_irqsave(&s_hostname_lock);
    uint32_t copy = s_hostname_len;
    if (copy >= n) copy = n - 1;
    for (uint32_t i = 0; i < copy; i++)
        out[i] = s_hostname[i];
    out[copy] = '\0';
    spin_unlock_irqrestore(&s_hostname_lock, fl);
}

/*
 * sys_sethostname — syscall 170
 *
 * arg1 = user pointer to new hostname bytes (not required to be NUL-terminated)
 * arg2 = length in bytes (not counting any NUL); must be <= HOSTNAME_MAX
 *
 * Requires CAP_KIND_POWER — same gate sys_reboot uses.  Hostname is
 * machine-identity state; only an authenticated admin should change it.
 *
 * Returns 0 on success; -EPERM if capability missing; -EINVAL if len > HOSTNAME_MAX;
 * -EFAULT if the user buffer is unreadable.
 */
uint64_t
sys_sethostname(uint64_t name_uptr, uint64_t len)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_POWER, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)1;   /* EPERM */

    if (len > HOSTNAME_MAX)
        return (uint64_t)-(int64_t)22;  /* EINVAL */

    if (len > 0 && !user_ptr_valid(name_uptr, len))
        return (uint64_t)-(int64_t)14;  /* EFAULT */

    char buf[HOSTNAME_MAX + 1];
    if (len > 0)
        copy_from_user(buf, (const void *)(uintptr_t)name_uptr, len);
    buf[len] = '\0';

    irqflags_t fl = spin_lock_irqsave(&s_hostname_lock);
    for (uint32_t i = 0; i < (uint32_t)len; i++)
        s_hostname[i] = buf[i];
    s_hostname[len] = '\0';
    s_hostname_len = (uint32_t)len;
    spin_unlock_irqrestore(&s_hostname_lock, fl);

    return 0;
}
