#ifndef AEGIS_SYSCALL_H
#define AEGIS_SYSCALL_H

#include <stdint.h>

/* Called from syscall_entry.asm with IF=0, DF=0, on kernel stack.
 * arg4 = r10 (Linux syscall ABI; r10 used instead of rcx because
 *        SYSCALL clobbers rcx with the return RIP).
 * arg5 = r8, arg6 = r9 per Linux convention.
 * Returns value placed in RAX on syscall return. */
uint64_t syscall_dispatch(uint64_t num, uint64_t arg1, uint64_t arg2,
                          uint64_t arg3, uint64_t arg4, uint64_t arg5,
                          uint64_t arg6);

#endif /* AEGIS_SYSCALL_H */
