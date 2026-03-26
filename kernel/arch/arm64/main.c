/*
 * main.c — ARM64 kernel entry point.
 *
 * Called from boot.S with MMU off. DTB physical address in x0.
 * Initializes serial, parses DTB for memory, runs PMM.
 */

#include "arch.h"
#include "printk.h"
#include "pmm.h"

void
kernel_main(uint64_t dtb_phys)
{
    arch_init();
    printk("[SERIAL] OK: PL011 UART initialized\n");

    arch_mm_init((void *)(uintptr_t)dtb_phys);
    pmm_init();

    printk("[AEGIS] ARM64 kernel ready.\n");
    printk("[AEGIS] System halted.\n");

    for (;;)
        arch_halt();
}
