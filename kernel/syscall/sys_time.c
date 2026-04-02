/* sys_time.c — Time-related syscalls: nanosleep, clock_gettime, clock_settime */
#include "sys_impl.h"

/*
 * sys_nanosleep — syscall 35
 * arg1 = user pointer to struct timespec { int64_t tv_sec; int64_t tv_nsec; }
 * arg2 = user pointer to remainder (NULL allowed; not populated)
 *
 * Sets sleep_deadline in the task struct and calls sched_block().
 * sched_tick auto-wakes the task when the deadline passes.
 * This properly removes the task from scheduling, unlike the old
 * sti;hlt;cli busy-wait which kept the task RUNNING and stole 50%
 * of CPU from other tasks.
 */
uint64_t
sys_nanosleep(uint64_t arg1, uint64_t arg2)
{
    (void)arg2;
    struct { int64_t tv_sec; int64_t tv_nsec; } ts;
    if (!user_ptr_valid(arg1, sizeof(ts)))
        return (uint64_t)-(int64_t)14; /* EFAULT */
    copy_from_user(&ts, (const void *)(uintptr_t)arg1, sizeof(ts));

    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000)
        return (uint64_t)-(int64_t)22; /* EINVAL */

    uint64_t ticks = (uint64_t)ts.tv_sec * 100ULL
                   + (uint64_t)ts.tv_nsec / 10000000ULL;
    if (ticks == 0 && ts.tv_nsec > 0) ticks = 1;

    aegis_task_t *cur = sched_current();
    cur->sleep_deadline = arch_get_ticks() + ticks;
    sched_block();
    return 0;
}

/*
 * sys_clock_gettime — syscall 228
 *
 * arg1 = clk_id  (CLOCK_REALTIME=0, CLOCK_MONOTONIC=1)
 * arg2 = user pointer to struct timespec { int64_t tv_sec; int64_t tv_nsec; }
 *
 * CLOCK_REALTIME returns wall-clock time (epoch_offset + ticks/100).
 * CLOCK_MONOTONIC returns raw PIT ticks (no epoch offset).
 * Returns 0 on success, negative errno on failure.
 */
uint64_t
sys_clock_gettime(uint64_t clk_id, uint64_t timespec_uptr)
{
    if (clk_id != 0 && clk_id != 1) return (uint64_t)-(int64_t)22; /* EINVAL */
    if (!user_ptr_valid(timespec_uptr, 16)) return (uint64_t)-(int64_t)14; /* EFAULT */

    int64_t tv_sec, tv_nsec;
    if (clk_id == 0) {
        /* CLOCK_REALTIME — wall clock set by NTP */
        uint64_t sec, nsec;
        arch_clock_gettime(&sec, &nsec);
        tv_sec  = (int64_t)sec;
        tv_nsec = (int64_t)nsec;
    } else {
        /* CLOCK_MONOTONIC — raw ticks, no epoch offset */
        uint64_t ticks = arch_get_ticks();
        tv_sec  = (int64_t)(ticks / 100ULL);
        tv_nsec = (int64_t)((ticks % 100ULL) * 10000000ULL);
    }
    copy_to_user((void *)(uintptr_t)timespec_uptr,       &tv_sec,  8);
    copy_to_user((void *)(uintptr_t)(timespec_uptr + 8), &tv_nsec, 8);
    return 0;
}

/*
 * sys_clock_settime — syscall 227
 *
 * arg1 = clk_id (CLOCK_REALTIME=0 only)
 * arg2 = user pointer to struct timespec { int64_t tv_sec; int64_t tv_nsec; }
 *
 * Sets the wall clock epoch offset so that subsequent clock_gettime(CLOCK_REALTIME)
 * returns real Unix time. Called by the chronos NTP daemon.
 * Returns 0 on success, negative errno on failure.
 */
uint64_t
sys_clock_settime(uint64_t clk_id, uint64_t timespec_uptr)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_NET_ADMIN, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)1; /* EPERM */
    if (clk_id != 0) return (uint64_t)-(int64_t)22; /* EINVAL: only CLOCK_REALTIME */
    if (!user_ptr_valid(timespec_uptr, 16)) return (uint64_t)-(int64_t)14; /* EFAULT */

    uint64_t sec;
    copy_from_user(&sec, (const void *)(uintptr_t)timespec_uptr, 8);
    arch_clock_settime(sec);
    return 0;
}
