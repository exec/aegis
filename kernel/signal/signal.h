#ifndef AEGIS_SIGNAL_H
#define AEGIS_SIGNAL_H

#include <stdint.h>

/* Signal numbers (Linux x86-64) */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)

/* musl struct sigaction layout (x86-64) */
typedef struct {
    void    (*sa_handler)(int);
    uint64_t sa_flags;
    void    (*sa_restorer)(void);   /* __restore_rt — must be non-NULL for RT signals */
    uint64_t sa_mask;               /* signals to mask while handler runs */
} k_sigaction_t;

_Static_assert(sizeof(k_sigaction_t) == 32, "k_sigaction_t must be 32 bytes (musl struct sigaction x86-64)");

/* gregset_t indices (Linux x86-64, REG_* in <sys/ucontext.h>) */
#define REG_R8       0
#define REG_R9       1
#define REG_R10      2
#define REG_R11      3
#define REG_R12      4
#define REG_R13      5
#define REG_R14      6
#define REG_R15      7
#define REG_RDI      8
#define REG_RSI      9
#define REG_RBP     10
#define REG_RBX     11
#define REG_RDX     12
#define REG_RAX     13
#define REG_RCX     14
#define REG_RSP     15
#define REG_RIP     16
#define REG_EFL     17
#define REG_CSGSFS  18
#define REG_ERR     19
#define REG_TRAPNO  20
#define REG_OLDMASK 21
#define REG_CR2     22

/*
 * rt_sigframe_t — signal frame placed on the user stack before a signal handler.
 *
 * Layout matches the Linux x86-64 ABI exactly:
 *   offset   0: pretcode      (8)  — pointer to __restore_rt (sa_restorer)
 *   offset   8: siginfo[128] (128) — struct siginfo (zeroed in Phase 17)
 *   offset 136: uc_flags       (8)
 *   offset 144: uc_link        (8)
 *   offset 152: uc_stack      (24)  — stack_t: ss_sp(8) + ss_flags(4) + pad(4) + ss_size(8)
 *   offset 176: gregs[23]    (184)  — mcontext_t gregset_t
 *   offset 360: fpregs         (8)  — pointer (zeroed; no FP state saved)
 *   offset 368: __reserved1[8](64)
 *   offset 432: uc_sigmask     (8)  — saved signal mask (low 64 bits)
 *   offset 440: _pad[120]    (120)  — pad sigset_t to 128 bytes
 *   total: 560 bytes
 *
 * musl's __restore_rt calls sys_rt_sigreturn (syscall 15) with RSP pointing
 * at the pretcode slot (offset 0). sys_rt_sigreturn reads the frame from
 * frame->user_rsp (the user stack pointer saved at syscall entry).
 */
typedef struct {
    uint64_t pretcode;              /* 0:   &__restore_rt (sa_restorer) */
    uint8_t  siginfo[128];          /* 8:   struct siginfo — zeroed */
    /* ucontext_t begins at offset 136 */
    uint64_t uc_flags;              /* 136: 0 */
    uint64_t uc_link;               /* 144: 0 */
    uint64_t uc_stack_sp;           /* 152: 0 */
    uint32_t uc_stack_flags;        /* 160: 0 */
    uint32_t uc_stack_pad;          /* 164: 0 (padding in stack_t) */
    uint64_t uc_stack_size;         /* 168: 0 */
    /* mcontext_t begins at offset 176 */
    int64_t  gregs[23];             /* 176: gregset_t (REG_R8..REG_CR2) */
    uint64_t fpregs;                /* 360: 0 (no FP state saved in Phase 17) */
    uint64_t __reserved1[8];        /* 368: 0 */
    /* uc_sigmask at offset 432 */
    uint64_t uc_sigmask;            /* 432: saved signal_mask */
    uint8_t  _uc_sigmask_pad[120];  /* 440: pad sigset_t to 128 bytes */
} rt_sigframe_t;

_Static_assert(sizeof(rt_sigframe_t) == 560, "rt_sigframe_t must be 560 bytes");

/* Magic return value from sys_rt_sigreturn — signals syscall_entry.asm
 * to skip signal check and do sysret directly (frame already patched). */
#define SIGRETURN_MAGIC 0xdeadbeefcafebabeULL

/* Forward declarations (avoid circular includes) */
struct cpu_state;
struct syscall_frame;

/* Deliver pending signals when returning to ring-3 via iretq.
 * Called from isr.asm between isr_dispatch and isr_post_dispatch.
 * Returns immediately if s->cs != 0x23 (ring-3 check). */
void signal_deliver(struct cpu_state *s);

/* Deliver pending signals when returning from a syscall via sysret.
 * frame:         syscall_frame_t * (for patching rip/rflags/user_rsp)
 * saved_rdi_ptr: pointer to the saved user rdi slot on kernel stack
 *                (signal_deliver_sysret writes signum there for handler delivery)
 * Returns 1 if a user handler was set up (rax should be set to 0 by caller),
 * returns 0 if no signal or SIG_DFL (sched_exit called — never reaches return). */
int signal_deliver_sysret(struct syscall_frame *frame, uint64_t *saved_rdi_ptr);

/* Send signal signum to the process with the given pid.
 * Safe to call from ISR context (IF=0, no allocation).
 * Calls sched_wake if target is TASK_BLOCKED. */
void signal_send_pid(uint32_t pid, int signum);

/* Return 1 if the current process has deliverable pending signals. */
int signal_check_pending(void);

#endif /* AEGIS_SIGNAL_H */
