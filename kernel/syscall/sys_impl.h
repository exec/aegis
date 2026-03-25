/* sys_impl.h — internal header for syscall TU communication.
 * Not exported beyond kernel/syscall/. Each sys_*.c includes this. */
#pragma once

#include "syscall.h"
#include "syscall_util.h"
#include "uaccess.h"
#include "sched.h"
#include "proc.h"
#include "signal.h"
#include "kbd.h"
#include "vfs.h"
#include "pipe.h"
#include "initrd.h"
#include "vmm.h"
#include "pmm.h"
#include "kva.h"
#include "elf.h"
#include "printk.h"
#include "arch.h"
#include "ext2.h"
#include "kbd_vfs.h"
#include <stdint.h>
#include <stddef.h>

extern void isr_post_dispatch(void);

/* ── Common defines ─────────────────────────────────────────────────────── */

#ifndef MAP_SHARED
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_ANONYMOUS   0x20
#define PROT_READ       0x01
#define PROT_WRITE      0x02
#endif

#define WNOHANG  1
#define WUNTRACED 2

#ifndef ARCH_SET_FS
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#endif

#ifndef ERANGE
#define ERANGE 34
#endif

#ifndef EBADF
#define EBADF 9
#endif

#ifndef ENOTDIR
#define ENOTDIR 20
#endif

/* ── Shared types ───────────────────────────────────────────────────────── */

typedef struct {
    uint64_t iov_base;   /* user pointer */
    uint64_t iov_len;
} aegis_iovec_t;

/* linux_dirent64 matches the Linux kernel structure exactly. */
typedef struct {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[1];  /* flexible — actual size determined by d_reclen */
} __attribute__((packed)) linux_dirent64_t;

#define USER_STACK_TOP_EXEC   0x7FFFFFFF000ULL
#define USER_STACK_NPAGES     4ULL
#define USER_STACK_BASE_EXEC  (USER_STACK_TOP_EXEC - USER_STACK_NPAGES * 4096ULL)

/* execve_argbuf_t — argv working storage allocated from kva.
 *
 * argv_bufs[64][256] alone is 16 KB — larger than a child process's
 * 4-page kernel stack.  Allocating from kva avoids the overflow.
 * Size: 64*256 + 65*8 + 64*8 = 17416 bytes → 5 kva pages.
 */
typedef struct {
    char     argv_bufs[64][256];
    char    *argv_ptrs[65];
    uint64_t str_ptrs[64];
} execve_argbuf_t;

#define EXECVE_ARGBUF_PAGES 5   /* ceil(17416 / 4096) */

/* ── Path helpers (sys_file.c) ──────────────────────────────────────────── */
int copy_path_from_user(char *kpath, uint64_t user_ptr, uint32_t bufsz);
int stat_copy_path(uint64_t user_ptr, char *out, uint32_t bufsz);

/* ── sys_io.c ───────────────────────────────────────────────────────────── */
uint64_t sys_write(uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t sys_writev(uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t sys_read(uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t sys_close(uint64_t a1);

/* ── sys_memory.c ───────────────────────────────────────────────────────── */
uint64_t sys_brk(uint64_t a1);
uint64_t sys_mmap(uint64_t a1, uint64_t a2, uint64_t a3,
                  uint64_t a4, uint64_t a5, uint64_t a6);
uint64_t sys_munmap(uint64_t a1, uint64_t a2);
uint64_t sys_mprotect(uint64_t a1, uint64_t a2, uint64_t a3);

/* ── sys_process.c ──────────────────────────────────────────────────────── */
uint64_t sys_exit(uint64_t a1);
uint64_t sys_exit_group(uint64_t a1);
uint64_t sys_getpid(void);
uint64_t sys_getppid(void);
uint64_t sys_set_tid_address(uint64_t a1);
uint64_t sys_set_robust_list(uint64_t a1, uint64_t a2);
uint64_t sys_arch_prctl(uint64_t a1, uint64_t a2);
uint64_t sys_fork(syscall_frame_t *frame);
uint64_t sys_waitpid(uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t sys_execve(syscall_frame_t *frame,
                    uint64_t a1, uint64_t a2, uint64_t a3);

/* ── sys_file.c ─────────────────────────────────────────────────────────── */
uint64_t sys_open(uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t sys_openat(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
uint64_t sys_getdents64(uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t sys_getcwd(uint64_t a1, uint64_t a2);
uint64_t sys_chdir(uint64_t a1);
uint64_t sys_stat(uint64_t a1, uint64_t a2);
uint64_t sys_fstat(uint64_t a1, uint64_t a2);
uint64_t sys_access(uint64_t a1, uint64_t a2);
uint64_t sys_nanosleep(uint64_t a1, uint64_t a2);
uint64_t sys_ioctl(uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t sys_fcntl(uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t sys_lseek(uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t sys_pipe2(uint64_t a1, uint64_t a2);
uint64_t sys_dup(uint64_t a1);
uint64_t sys_dup2(uint64_t a1, uint64_t a2);
uint64_t sys_mkdir(uint64_t a1, uint64_t a2);
uint64_t sys_unlink(uint64_t a1);
uint64_t sys_rename(uint64_t a1, uint64_t a2);
uint64_t sys_getuid(void);
uint64_t sys_geteuid(void);
uint64_t sys_getgid(void);
uint64_t sys_getegid(void);

/* ── sys_signal.c ───────────────────────────────────────────────────────── */
uint64_t sys_rt_sigaction(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
uint64_t sys_rt_sigprocmask(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
uint64_t sys_rt_sigreturn(syscall_frame_t *frame);
uint64_t sys_kill(uint64_t a1, uint64_t a2);
uint64_t sys_setfg(uint64_t a1);

/* ── Process group / session / resource syscalls (sys_process.c) ─────────── */
uint64_t sys_setpgid(uint64_t pid_arg, uint64_t pgid_arg);
uint64_t sys_getpgrp(void);
uint64_t sys_setsid(void);
uint64_t sys_getpgid(uint64_t pid_arg);
uint64_t sys_umask(uint64_t mask);
uint64_t sys_getrlimit(uint64_t resource, uint64_t rlim_ptr);
