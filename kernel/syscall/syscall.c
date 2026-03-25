/* syscall.c — Syscall dispatch table.
 * Implementation split into sys_io.c, sys_memory.c, sys_process.c,
 * sys_file.c, sys_signal.c. */
#include "sys_impl.h"

uint64_t
syscall_dispatch(syscall_frame_t *frame, uint64_t num,
                 uint64_t arg1, uint64_t arg2, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    switch (num) {
    case  0: return sys_read(arg1, arg2, arg3);
    case  1: return sys_write(arg1, arg2, arg3);
    case  2: return sys_open(arg1, arg2, arg3);
    case  3: return sys_close(arg1);
    case  4: return sys_stat(arg1, arg2);
    case  5: return sys_fstat(arg1, arg2);
    case  6: return sys_stat(arg1, arg2);   /* lstat = stat (no symlinks) */
    case  8: return sys_lseek(arg1, arg2, arg3);
    case 21: return sys_access(arg1, arg2);
    case 35: return sys_nanosleep(arg1, arg2);
    case 10: return sys_mprotect(arg1, arg2, arg3);
    case 16: return sys_ioctl(arg1, arg2, arg3);
    case 22: return sys_pipe2(arg1, 0); /* pipe(2) = pipe2(pipefd, 0) */
    case 32: return sys_dup(arg1);
    case 33: return sys_dup2(arg1, arg2);
    case  9: return sys_mmap(arg1, arg2, arg3, arg4, arg5, arg6);
    case 11: return sys_munmap(arg1, arg2);
    case 12: return sys_brk(arg1);
    case 72: return sys_fcntl(arg1, arg2, arg3);
    case 13: return sys_rt_sigaction(arg1, arg2, arg3, arg4);
    case 14: return sys_rt_sigprocmask(arg1, arg2, arg3, arg4);
    case 15: return sys_rt_sigreturn(frame);
    case 20: return sys_writev(arg1, arg2, arg3);
    case 39: return sys_getpid();
    case 57: return sys_fork(frame);
    case 59: return sys_execve(frame, arg1, arg2, arg3);
    case 60: return sys_exit(arg1);
    case 61: return sys_waitpid(arg1, arg2, arg3);
    case 62: return sys_kill(arg1, arg2);
    case 360: return sys_setfg(arg1);
    case  79: return sys_getcwd(arg1, arg2);
    case  80: return sys_chdir(arg1);
    case 217: return sys_getdents64(arg1, arg2, arg3);
    case 102: return sys_getuid();
    case 104: return sys_getgid();
    case 105: return sys_setuid(arg1);
    case 106: return sys_setgid(arg1);
    case 107: return sys_geteuid();
    case 108: return sys_getegid();
    case 110: return sys_getppid();
    case  95: return sys_umask(arg1);
    case  97: return sys_getrlimit(arg1, arg2);
    case 109: return sys_setpgid(arg1, arg2);
    case 111: return sys_getpgrp();
    case 112: return sys_setsid();
    case 121: return sys_getpgid(arg1);
    case  63: return sys_uname(arg1);
    case 158: return sys_arch_prctl(arg1, arg2);
    case 218: return sys_set_tid_address(arg1);
    case 231: return sys_exit_group(arg1);
    case 273: return sys_set_robust_list(arg1, arg2);
    case 293: return sys_pipe2(arg1, arg2);
    case  82: return sys_rename(arg1, arg2);
    case  83: return sys_mkdir(arg1, arg2);
    case  87: return sys_unlink(arg1);
    case 257: return sys_openat(arg1, arg2, arg3, arg4);
    default:
        return (uint64_t)-(int64_t)38;   /* ENOSYS */
    }
}
