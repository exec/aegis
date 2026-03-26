#ifndef SYSCALL_UTIL_H
#define SYSCALL_UTIL_H

#include <stdint.h>
#include "arch.h"

/* user_ptr_valid — return 1 if [addr, addr+len) lies entirely within the
 * canonical user address space, 0 otherwise.
 * For len=0, validates that addr itself is a canonical user address (does
 * NOT unconditionally pass — a kernel addr with len=0 still returns 0).
 * Overflow-safe: addr <= USER_ADDR_MAX - len avoids addr+len wraparound. */
static inline int
user_ptr_valid(uint64_t addr, uint64_t len)
{
    return len <= USER_ADDR_MAX && addr <= USER_ADDR_MAX - len;
}

#endif /* SYSCALL_UTIL_H */
