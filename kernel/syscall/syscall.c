#include "syscall.h"
#include "syscall_util.h"
#include "uaccess.h"
#include "sched.h"
#include "printk.h"
#include <stdint.h>

/*
 * sys_write — syscall 1
 *
 * arg1 = fd (ignored: all output goes to printk)
 * arg2 = user virtual address of buffer
 * arg3 = byte count
 *
 * Returns byte count on success, -14 (EFAULT) if [arg2, arg2+arg3) is not
 * a canonical user-space range.
 *
 * copy_from_user copies up to 256 bytes at a time into a kernel stack buffer
 * (one stac/clac pair per chunk), then feeds each character to printk
 * outside the AC window.
 */
static uint64_t
sys_write(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    (void)arg1;
    if (!user_ptr_valid(arg2, arg3))
        return (uint64_t)-14;   /* EFAULT */
    const char *s = (const char *)(uintptr_t)arg2;
    char kbuf[256];
    uint64_t offset = 0;
    while (offset < arg3) {
        uint64_t n = arg3 - offset;
        if (n > sizeof(kbuf)) n = sizeof(kbuf);
        copy_from_user(kbuf, s + offset, n);
        uint64_t j;
        for (j = 0; j < n; j++)
            printk("%c", kbuf[j]);
        offset += n;
    }
    return arg3;
}

/*
 * sys_exit — syscall 60
 *
 * arg1 = exit code (ignored for Phase 5)
 * Calls sched_exit() which never returns.
 */
static uint64_t
sys_exit(uint64_t arg1)
{
    (void)arg1;
    sched_exit();
    __builtin_unreachable();
}

uint64_t
syscall_dispatch(uint64_t num, uint64_t arg1,
                 uint64_t arg2, uint64_t arg3)
{
    switch (num) {
    case 1:  return sys_write(arg1, arg2, arg3);
    case 60: return sys_exit(arg1);
    default: return (uint64_t)-1;   /* ENOSYS */
    }
}
