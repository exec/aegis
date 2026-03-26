/*
 * main.c — ARM64 kernel entry point.
 *
 * Called from boot.S with MMU on (identity-mapped). DTB pointer in x0.
 */

#include "arch.h"
#include "printk.h"
#include "pmm.h"
#include "vmm.h"
#include "kva.h"
#include "sched.h"
#include "vfs.h"
#include "console.h"
#include "cap.h"

/* From gic.c */
void gic_init(void);
void timer_init(void);

/* From stubs.c (backed by uart_pl011.c RX) */
void kbd_init(void);

/* From proc.c */
void proc_spawn_init(void);

/* From vectors.S */
extern char exception_vectors[];

static void
install_vectors(void)
{
    __asm__ volatile("msr vbar_el1, %0" : : "r"(exception_vectors));
    __asm__ volatile("isb");
}

/* Task 0: idle — enables interrupts and halts until next tick. */
static void
task_idle(void)
{
    arch_enable_irq();
    for (;;)
        arch_halt();
}

void
kernel_main(uint64_t dtb_phys)
{
    arch_init();
    printk("[SERIAL] OK: PL011 UART initialized\n");

    /* Convert DTB physical address to TTBR1 VA.
     * DTB is in device region (PA 0x0+), mapped at 0xFFFF000000000000+. */
    arch_mm_init((void *)(uintptr_t)(dtb_phys + KERN_VA_OFFSET));
    pmm_init();
    vmm_init();
    kva_init();

    install_vectors();
    gic_init();
    timer_init();
    kbd_init();    /* PL011 RX interrupt for serial input */

    cap_init();
    vfs_init();
    console_init();

    sched_init();
    sched_spawn(task_idle);
    proc_spawn_init();

    sched_start();

    __builtin_unreachable();
}
