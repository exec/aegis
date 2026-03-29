#ifndef AEGIS_SCHED_H
#define AEGIS_SCHED_H

#include <stdint.h>
#include "smp.h"

#define TASK_RUNNING  0U
#define TASK_BLOCKED  1U
#define TASK_ZOMBIE   2U
#define TASK_STOPPED  3U

typedef struct aegis_task_t {
    uint64_t             sp;               /* MUST be first — ctx_switch reads [rdi+0] */
    uint8_t             *stack_base;       /* bottom of kva-allocated stack (freed on exit via kva_free_pages) */
    uint64_t             kernel_stack_top; /* RSP0 value: kernel stack top for this task */
    uint32_t             tid;              /* task ID */
    uint8_t              is_user;          /* 1 = user process (aegis_process_t), 0 = kernel task */
    uint64_t             stack_pages;      /* kva pages allocated for this task's kernel stack */
    uint32_t             state;        /* TASK_RUNNING=0 TASK_BLOCKED=1 TASK_ZOMBIE=2 */
    uint32_t             waiting_for;  /* PID this task waits for; 0=any child */
    uint64_t             fs_base;          /* per-thread TLS base (IA32_FS_BASE / TPIDR_EL0) */
    uint64_t             clear_child_tid;  /* user VA: write 0 + futex_wake on exit */
    uint64_t             sleep_deadline;   /* PIT tick when nanosleep expires; 0 = not sleeping */
    struct aegis_task_t *next;             /* circular linked list */
} aegis_task_t;

/* Initialize the run queue. No tasks yet. */
void sched_init(void);

/* Allocate a TCB and 16KB stack from PMM; wire fn as entry point; add to queue. */
void sched_spawn(void (*fn)(void));

/* Print [SCHED] OK line, then switch directly into the first task via a
 * dummy TCB (one-way ctx_switch). Does not return. Each task enables
 * interrupts on entry via the arch interrupt-enable primitive. */
void sched_start(void);

/* Called by pit_handler on each timer tick.
 * Advances current task and calls ctx_switch. */
void sched_tick(void);

/* Add a pre-initialized TCB to the run queue.
 * Used by proc_spawn to insert a user process without duplicating
 * the list-insertion logic from sched_spawn. */
void sched_add(aegis_task_t *task);

/* Remove the current task from the run queue and switch to the next task.
 * Called from sys_exit (syscall 60). Does not return.
 * TCB and kernel stack are freed via deferred kva_free_pages at the next sched_exit call. */
void sched_exit(void);

/* Returns the currently running task for this CPU via per-CPU GS.base. */
static inline aegis_task_t *
sched_current(void)
{
    return percpu_current();
}

/* sched_block — mark current task TASK_BLOCKED and yield.
 * Unlinks current from the run queue; switches to next RUNNING task.
 * REQUIRES: at least one other TASK_RUNNING task in the queue.
 *            Guaranteed by task_idle which never blocks or exits. */
void sched_block(void);

/* sched_wake — mark task TASK_RUNNING and re-add to run queue.
 * Inserts before current so the woken task runs next tick.
 * Called from sched_exit when the parent is found waiting. IF=0. */
void sched_wake(aegis_task_t *task);

/* sched_stop — transition a task to TASK_STOPPED.
 * If task == sched_current() (self-stop): mirrors sched_block exactly —
 * sets state, advances to next TASK_RUNNING via percpu, updates TSS/FS.base,
 * calls ctx_switch. Execution resumes when SIGCONT calls sched_resume and
 * sched_tick next schedules this task.
 * If task != sched_current(): sets task->state = TASK_STOPPED only;
 * sched_tick will skip it on next preemption. Must be called with IF=0. */
void sched_stop(aegis_task_t *task);

/* sched_resume — transition TASK_STOPPED (or TASK_BLOCKED) back to TASK_RUNNING.
 * Mirrors sched_wake: task->state = TASK_RUNNING. No list re-insertion needed. */
void sched_resume(aegis_task_t *task);

/* sched_yield_to_next — advance percpu current to the next RUNNING task
 * and ctx_switch. Used by the zombie path in sched_exit.
 * The zombie's kernel stack is still live during ctx_switch; the caller
 * must not touch task state after sched_yield_to_next returns (it returns
 * in the new task's context, not the zombie's). */
void sched_yield_to_next(void);

#endif /* AEGIS_SCHED_H */
