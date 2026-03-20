#ifndef AEGIS_SYSCALL_H
#define AEGIS_SYSCALL_H

#include <stdint.h>

/* Called from syscall_entry.asm with IF=0, DF=0, on kernel stack.
 * Returns value placed in RAX on syscall return. */
uint64_t syscall_dispatch(uint64_t num, uint64_t arg1,
                          uint64_t arg2, uint64_t arg3);

#endif /* AEGIS_SYSCALL_H */
