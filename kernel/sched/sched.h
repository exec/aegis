#ifndef AEGIS_SCHED_H
#define AEGIS_SCHED_H

#include <stdint.h>

typedef struct aegis_task_t {
    uint64_t             rsp;         /* MUST be first — ctx_switch reads [rdi+0] */
    uint8_t             *stack_base;  /* bottom of PMM-allocated stack (for future cleanup) */
    uint32_t             tid;         /* task ID */
    struct aegis_task_t *next;        /* circular linked list */
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

#endif /* AEGIS_SCHED_H */
