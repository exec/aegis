/* sys_cap.c — Capability syscalls: grant_exec, grant_runtime, query */
#include "sys_impl.h"

/*
 * sys_cap_grant_exec — syscall 361
 *
 * Pre-registers a capability in exec_caps[].  When the calling process
 * subsequently calls sys_execve, the exec_caps are applied to the new
 * image's cap table after the baseline (VFS_OPEN/READ/WRITE) is set,
 * then exec_caps[] is zeroed.  Allows a parent to delegate extra caps
 * (e.g. CAP_KIND_AUTH) to a child that will exec a service binary.
 *
 * Requires: CAP_KIND_CAP_GRANT with CAP_RIGHTS_READ in caller's cap table.
 */
uint64_t
sys_cap_grant_exec(uint64_t kind_arg, uint64_t rights_arg)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_CAP_GRANT, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)ENOCAP;
    uint32_t kind   = (uint32_t)kind_arg;
    uint32_t rights = (uint32_t)rights_arg;
    if (kind == CAP_KIND_NULL || kind >= CAP_TABLE_SIZE)
        return (uint64_t)-(int64_t)22; /* EINVAL */
    /* H6: Caller must hold the cap being pre-registered — cannot grant
     * capabilities it does not possess. */
    if (cap_check(proc->caps, CAP_TABLE_SIZE, kind, rights) != 0)
        return (uint64_t)-(int64_t)ENOCAP;
    int r = cap_grant(proc->exec_caps, CAP_TABLE_SIZE, kind, rights);
    if (r < 0) return (uint64_t)-(int64_t)12; /* ENOMEM/table full */
    return 0;
}

/*
 * sys_cap_grant — syscall 363
 *
 * Grant a capability to a running process by PID.  The caller must hold
 * CAP_KIND_CAP_DELEGATE and the specific capability being granted.
 */
uint64_t
sys_cap_grant_runtime(uint64_t target_pid, uint64_t kind_arg, uint64_t rights_arg)
{
    aegis_process_t *caller = (aegis_process_t *)sched_current();

    /* Caller must hold CAP_DELEGATE */
    if (cap_check(caller->caps, CAP_TABLE_SIZE,
                  CAP_KIND_CAP_DELEGATE, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    uint32_t kind   = (uint32_t)kind_arg;
    uint32_t rights = (uint32_t)rights_arg;

    if (kind == CAP_KIND_NULL || kind >= CAP_TABLE_SIZE)
        return (uint64_t)-(int64_t)22; /* EINVAL */

    /* H5: Caller must hold the cap kind being granted with at least the
     * rights being delegated — prevents escalation beyond caller's own
     * authority. */
    if (cap_check(caller->caps, CAP_TABLE_SIZE, kind, rights) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    /* Look up target process */
    aegis_process_t *target = proc_find_by_pid((uint32_t)target_pid);
    if (!target)
        return (uint64_t)-(int64_t)3; /* ESRCH */

    /* Grant the cap into target's table */
    int r = cap_grant(target->caps, CAP_TABLE_SIZE, kind, rights);
    if (r < 0)
        return (uint64_t)-(int64_t)28; /* ENOSPC — cap table full */

    return (uint64_t)r; /* slot index */
}

/*
 * sys_cap_query — syscall 362
 * Returns the capability table of a process.
 *   pid == 0: own caps (always allowed)
 *   pid != 0: target's caps (requires CAP_KIND_CAP_QUERY)
 * Copies cap_slot_t entries to user buffer.
 * Returns number of slots copied, or negative errno.
 */
uint64_t
sys_cap_query(uint64_t pid_arg, uint64_t buf_uptr, uint64_t buflen)
{
    aegis_process_t *caller = (aegis_process_t *)sched_current();
    if (!sched_current()->is_user)
        return (uint64_t)-(int64_t)1;  /* EPERM */

    aegis_process_t *target;

    if (pid_arg == 0) {
        target = caller;
    } else {
        if (cap_check(caller->caps, CAP_TABLE_SIZE,
                      CAP_KIND_CAP_QUERY, CAP_RIGHTS_READ) != 0)
            return (uint64_t)-(int64_t)ENOCAP;

        target = proc_find_by_pid((uint32_t)pid_arg);
        if (!target)
            return (uint64_t)-(int64_t)3;  /* ESRCH */
    }

    uint64_t copy_bytes = CAP_TABLE_SIZE * sizeof(cap_slot_t);
    if (buflen < copy_bytes)
        copy_bytes = buflen;

    uint64_t nslots = copy_bytes / sizeof(cap_slot_t);
    if (nslots == 0)
        return 0;

    copy_bytes = nslots * sizeof(cap_slot_t);

    if (!user_ptr_valid(buf_uptr, copy_bytes))
        return (uint64_t)-(int64_t)14;  /* EFAULT */
    copy_to_user((void *)buf_uptr, target->caps, copy_bytes);

    return nslots;
}
