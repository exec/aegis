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

/* From gic.c */
void gic_init(void);
void timer_init(void);

/* From vectors.S */
extern char exception_vectors[];

static void
install_vectors(void)
{
    __asm__ volatile("msr vbar_el1, %0" : : "r"(exception_vectors));
    __asm__ volatile("isb");
}

void
kernel_main(uint64_t dtb_phys)
{
    arch_init();
    printk("[SERIAL] OK: PL011 UART initialized\n");

    arch_mm_init((void *)(uintptr_t)dtb_phys);
    pmm_init();
    vmm_init();
    kva_init();

    install_vectors();
    gic_init();
    timer_init();

    /* Enable IRQs — timer will start ticking */
    arch_enable_irq();

    /* Wait a few ticks to prove the timer works */
    while (arch_get_ticks() < 3)
        arch_halt();

    printk("[AEGIS] ARM64 kernel ready, ticks=%lu\n", arch_get_ticks());
    printk("[AEGIS] System halted.\n");

    for (;;)
        arch_halt();
}
