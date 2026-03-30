/* smp.h — SMP per-CPU data structures (ARM64)
 *
 * ARM64 equivalent of kernel/arch/x86_64/smp.h.
 * Uses TPIDR_EL1 as the per-CPU base pointer (analogous to x86 GS.base).
 */
#ifndef AEGIS_SMP_H
#define AEGIS_SMP_H

#include <stdint.h>

/* Forward declaration to avoid circular include with sched.h */
struct aegis_task_t;

#define MAX_CPUS 16

/* Per-CPU data structure. TPIDR_EL1 points here in kernel mode.
 * FIELD OFFSETS (assembly uses ldr x, [tpidr_el1, #OFFSET]):
 *   self            = +0
 *   cpu_id          = +8
 *   affinity_id     = +9   (ARM64 equivalent of LAPIC ID)
 *   current_task    = +16
 *   kernel_stack    = +24
 *   user_sp_scratch = +32
 *   ticks           = +40
 */
typedef struct percpu {
    struct percpu         *self;              /* offset 0 */
    uint8_t                cpu_id;            /* offset 8 */
    uint8_t                affinity_id;       /* offset 9 (MPIDR affinity) */
    uint8_t                _pad[6];           /* offset 10-15 */
    struct aegis_task_t   *current_task;      /* offset 16 */
    uint64_t               kernel_stack;      /* offset 24 */
    uint64_t               user_sp_scratch;   /* offset 32 */
    uint64_t               ticks;             /* offset 40 */
    /* Deferred dying task cleanup — per-CPU to avoid SMP race on globals. */
    void                  *prev_dying_tcb;    /* offset 48 */
    void                  *prev_dying_stack;  /* offset 56 */
    uint64_t               prev_dying_stack_pages; /* offset 64 */
} percpu_t;

/* Assembly-visible offset constants */
#define PERCPU_SELF            0
#define PERCPU_CPU_ID          8
#define PERCPU_CURRENT_TASK    16
#define PERCPU_KERNEL_STACK    24
#define PERCPU_USER_SP         32
#define PERCPU_TICKS           40

extern percpu_t g_percpu[MAX_CPUS];
extern uint32_t g_cpu_count;
extern volatile uint8_t g_ap_online[MAX_CPUS];

void smp_percpu_init_bsp(void);
void smp_start_aps(void);
void ap_entry(void);

/* Return pointer to the current CPU's percpu_t via TPIDR_EL1. */
static inline percpu_t *
percpu_self(void)
{
    percpu_t *p;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(p));
    return p;
}

static inline struct aegis_task_t *
percpu_current(void)
{
    struct aegis_task_t *t;
    percpu_t *p;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(p));
    t = p->current_task;
    return t;
}

static inline void
percpu_set_current(struct aegis_task_t *t)
{
    percpu_t *p;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(p));
    p->current_task = t;
}

#endif /* AEGIS_SMP_H */
