/* sys_identity.c — Identity, info, session, and resource syscalls */
#include "sys_impl.h"
#include "sched.h"
#include "proc.h"
#include "arch.h"
#include "ext2.h"
#include "printk.h"
#include "acpi.h"

uint64_t
sys_getpid(void)
{
    aegis_task_t *task = sched_current();
    if (!task->is_user) return 0;
    return (uint64_t)((aegis_process_t *)task)->tgid;
}

uint64_t
sys_gettid(void)
{
    aegis_task_t *task = sched_current();
    if (!task->is_user) return 0;
    return (uint64_t)((aegis_process_t *)task)->pid;
}

/*
 * sys_getppid — syscall 110
 *
 * Returns the parent PID of the calling process.
 * No capability gate — a process may always query its own parent.
 */
uint64_t
sys_getppid(void)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    return (uint64_t)proc->ppid;
}

uint64_t
sys_set_tid_address(uint64_t arg1)
{
    aegis_task_t *task = sched_current();
    if (arg1 && arg1 >= 0xFFFF800000000000ULL)
        return (uint64_t)-(int64_t)14;  /* EFAULT — reject kernel addresses */
    task->clear_child_tid = arg1;
    if (!task->is_user) return 1;
    return (uint64_t)((aegis_process_t *)task)->pid;
}

uint64_t sys_set_robust_list(uint64_t a, uint64_t b)
{
    (void)a; (void)b;
    return 0;
}

/*
 * sys_arch_prctl — syscall 158
 *
 * arg1 = code
 * arg2 = addr
 *
 * ARCH_SET_FS (0x1002): set FS.base to addr. Writes IA32_FS_BASE MSR
 *   and saves to proc->fs_base.
 * ARCH_GET_FS (0x1003): write current fs_base to *addr.
 * All other codes: return -EINVAL.
 */
uint64_t
sys_arch_prctl(uint64_t arg1, uint64_t arg2)
{
    if (arg1 == ARCH_SET_FS) {
        /* Reject kernel addresses — user process must not set FS base to
         * kernel space. Corrupts musl TLS → cascading garbage syscalls. */
        if (arg2 >= 0xFFFF800000000000ULL)
            return (uint64_t)-(int64_t)14;  /* EFAULT */
        sched_current()->fs_base = arg2;
        arch_set_fs_base(arg2);
        return 0;
    }
    if (arg1 == ARCH_GET_FS) {
        if (!user_ptr_valid(arg2, sizeof(uint64_t)))
            return (uint64_t)-(int64_t)14;   /* EFAULT */
        copy_to_user((void *)(uintptr_t)arg2, &sched_current()->fs_base,
                     sizeof(uint64_t));
        return 0;
    }
    return (uint64_t)-(int64_t)22;   /* EINVAL */
}

/* sys_setpgid — syscall 109: set pgid of process pid to pgid.
 * pid=0 means current process. pgid=0 means use target's pid.
 * Allows creating own group (pgid == pid) or joining existing group in same session. */
uint64_t
sys_setpgid(uint64_t pid_arg, uint64_t pgid_arg)
{
    aegis_process_t *caller = (aegis_process_t *)sched_current();
    uint32_t pid  = (uint32_t)pid_arg;
    uint32_t pgid = (uint32_t)pgid_arg;

    aegis_process_t *target = caller;
    if (pid != 0 && pid != caller->pid) {
        target = proc_find_by_pid(pid);
        if (!target)
            return (uint64_t)-(int64_t)3; /* ESRCH */
        /* Can only setpgid on self or direct child */
        if (target->ppid != caller->pid)
            return (uint64_t)-(int64_t)1; /* EPERM */
    }

    if (pgid == 0)
        pgid = target->pid;

    /* Always allow: pgid == target->pid (create own group) */
    if (pgid != target->pid) {
        /* Verify pgid belongs to a process in the same session */
        int found = 0;
        aegis_task_t *t = sched_current();
        aegis_task_t *start = t;
        do {
            if (t->is_user) {
                aegis_process_t *p = (aegis_process_t *)t;
                if (p->pgid == pgid && p->sid == target->sid) {
                    found = 1;
                    break;
                }
            }
            t = t->next;
        } while (t != start);
        if (!found)
            return (uint64_t)-(int64_t)1; /* EPERM */
    }

    target->pgid = pgid;
    return 0;
}

/* sys_getpgrp — syscall 111: return current->pgid */
uint64_t
sys_getpgrp(void)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    return (uint64_t)proc->pgid;
}

/* sys_setsid — syscall 112: create new session.
 * Fails with EPERM if caller is already a process group leader. */
uint64_t
sys_setsid(void)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    /* POSIX: EPERM if already a process group leader */
    if (proc->pgid == proc->pid)
        return (uint64_t)-(int64_t)1; /* EPERM */
    proc->sid  = proc->pid;
    proc->pgid = proc->pid;
    return (uint64_t)proc->pid;
}

/* sys_getpgid — syscall 121: return pgid of process pid (0 = self). */
uint64_t
sys_getpgid(uint64_t pid_arg)
{
    aegis_process_t *caller = (aegis_process_t *)sched_current();
    uint32_t pid = (uint32_t)pid_arg;
    if (pid == 0)
        return (uint64_t)caller->pgid;

    aegis_task_t *t = sched_current()->next;
    while (t != sched_current()) {
        if (t->is_user) {
            aegis_process_t *p = (aegis_process_t *)t;
            if (p->pid == pid)
                return (uint64_t)p->pgid;
        }
        t = t->next;
    }
    return (uint64_t)-(int64_t)3; /* ESRCH */
}

/* sys_umask — syscall 95: set file creation mask; return previous value.
 * Not yet wired to ext2 create. */
uint64_t
sys_umask(uint64_t mask)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    uint32_t old = proc->umask;
    proc->umask  = (uint32_t)(mask & 0777U);
    return (uint64_t)old;
}

/* sys_getrlimit — syscall 97: return {RLIM_INFINITY, RLIM_INFINITY}.
 * struct rlimit = {uint64_t rlim_cur, rlim_max} = 16 bytes. */
uint64_t
sys_getrlimit(uint64_t resource, uint64_t rlim_ptr)
{
    (void)resource;
    if (!user_ptr_valid(rlim_ptr, 16))
        return (uint64_t)-(int64_t)14; /* EFAULT */
    uint64_t inf[2] = { ~0ULL, ~0ULL };
    copy_to_user((void *)(uintptr_t)rlim_ptr, inf, 16);
    return 0;
}

/*
 * sys_uname — syscall 63
 * arg1 = user pointer to struct utsname (6 x 65-byte char arrays).
 * Returns kernel identity strings; oksh uses these for $HOSTNAME and PS1.
 */
uint64_t
sys_uname(uint64_t buf_uptr)
{
    /* struct utsname: sysname[65] nodename[65] release[65]
     *                 version[65] machine[65] domainname[65] */
    char uts[6 * 65];
    if (!user_ptr_valid(buf_uptr, sizeof(uts)))
        return (uint64_t)-(int64_t)14; /* EFAULT */
    __builtin_memset(uts, 0, sizeof(uts));
    __builtin_memcpy(uts + 0*65,  "Aegis",   5); /* sysname    */
    __builtin_memcpy(uts + 1*65,  "aegis",   5); /* nodename   */
    __builtin_memcpy(uts + 2*65,  "1.0.0",   5); /* release    */
    __builtin_memcpy(uts + 3*65,  "#1",      2); /* version    */
#ifdef __aarch64__
    __builtin_memcpy(uts + 4*65,  "aarch64", 7); /* machine    */
#else
    __builtin_memcpy(uts + 4*65,  "x86_64",  6); /* machine    */
#endif
    __builtin_memcpy(uts + 5*65,  "(none)",  6); /* domainname */
    copy_to_user((void *)(uintptr_t)buf_uptr, uts, sizeof(uts));
    return 0;
}

/*
 * sys_setuid — syscall 105
 * Phase 25.7: gate uses CAP_KIND_SETUID — uid=0 has no ambient authority.
 */
uint64_t
sys_setuid(uint64_t uid_arg)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_SETUID, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)13; /* EACCES */
    proc->uid = (uint32_t)uid_arg;
    return 0;
}

/*
 * sys_setgid — syscall 106
 * Phase 25.7: gate uses CAP_KIND_SETUID — uid=0 has no ambient authority.
 */
uint64_t
sys_setgid(uint64_t gid_arg)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_SETUID, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)13; /* EACCES */
    proc->gid = (uint32_t)gid_arg;
    return 0;
}

/* Identity syscalls — moved from sys_file.c. */
uint64_t sys_getuid(void) {
    aegis_process_t *p = (aegis_process_t *)sched_current();
    return p ? (uint64_t)p->uid : 0;
}
uint64_t sys_geteuid(void) { return sys_getuid(); }
uint64_t sys_getgid(void) {
    aegis_process_t *p = (aegis_process_t *)sched_current();
    return p ? (uint64_t)p->gid : 0;
}
uint64_t sys_getegid(void) { return sys_getgid(); }

/*
 * sys_reboot — syscall 169
 *
 * cmd=0: ACPI S5 power off
 * cmd=1: keyboard controller reset (reboot)
 *
 * Requires CAP_KIND_POWER — granted via kernel cap policy.
 */
uint64_t
sys_reboot(uint64_t cmd)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_POWER, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    if (cmd == 0) {
        /* Power off */
#ifdef __x86_64__
        acpi_do_poweroff();
#else
        arch_debug_exit(0);    /* QEMU virt: write to power device */
#endif
        arch_disable_irq();
        for (;;) arch_halt();
    } else if (cmd == 1) {
        /* Reboot */
        ext2_sync();
        printk("[AEGIS] Rebooting...\n");
#ifdef __x86_64__
        /* 8042 keyboard-controller pulse reset */
        __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
#else
        /* ARM64: PSCI SYSTEM_RESET would go here; for now fall through to halt */
        arch_debug_exit(1);
#endif
        arch_disable_irq();
        for (;;) arch_halt();
    }
    return (uint64_t)-(int64_t)22;  /* EINVAL */
}
