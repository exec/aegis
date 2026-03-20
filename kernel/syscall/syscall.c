#include "syscall.h"
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
 * Prints via printk character-by-character because printk does not
 * support %.*s.  Direct dereference of arg2 is safe: SMAP is not
 * enabled and the user PML4 shares the kernel higher-half, so the
 * kernel can read user addresses while the user PML4 is loaded in CR3.
 *
 * PHASE 5 SECURITY DEBT: no pointer validation. A malicious user
 * could pass a kernel-space address as arg2 and read arbitrary kernel
 * memory via printk. Phase 6 must add:
 *   (a) bounds check: arg2 + arg3 <= 0x00007FFFFFFFFFFF
 *   (b) SMAP enable so unintentional kernel→user dereferences fault.
 */
static uint64_t
sys_write(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    (void)arg1;
    const char *s = (const char *)(uintptr_t)arg2;
    uint64_t i;
    for (i = 0; i < arg3; i++)
        printk("%c", s[i]);
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
