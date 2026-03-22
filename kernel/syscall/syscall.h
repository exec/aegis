#ifndef AEGIS_SYSCALL_H
#define AEGIS_SYSCALL_H

#include <stdint.h>

/* syscall_frame_t — saved user register state on the kernel stack.
 * Field order matches the push sequence in syscall_entry.asm.
 * The last-pushed value is at the lowest address (= struct base).
 * sys_execve writes frame->rip and frame->user_rsp to redirect sysret. */
typedef struct syscall_frame {
    uint64_t r10;       /* offset +0:  saved user r10 (Linux arg4) */
    uint64_t r9;        /* offset +8:  saved user r9  (Linux arg6) */
    uint64_t r8;        /* offset +16: saved user r8  (Linux arg5) */
    uint64_t rflags;    /* offset +24: saved user RFLAGS (r11 at entry) */
    uint64_t rip;       /* offset +32: saved user RIP  (rcx at entry) */
    uint64_t user_rsp;  /* offset +40: saved user RSP (pushed first) */
} syscall_frame_t;

uint64_t syscall_dispatch(syscall_frame_t *frame, uint64_t num,
                          uint64_t arg1, uint64_t arg2, uint64_t arg3,
                          uint64_t arg4, uint64_t arg5, uint64_t arg6);

#endif /* AEGIS_SYSCALL_H */
