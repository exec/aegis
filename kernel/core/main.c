#include "arch.h"
#include "printk.h"
#include "cap.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include <stdint.h>

/*
 * kernel_main — top-level kernel entry point.
 *
 * Called from boot.asm after long mode is established and the stack
 * is set up at boot_stack_top (higher-half virtual address).
 *
 * Arguments (System V AMD64 ABI, set in boot.asm):
 *   mb_magic — multiboot2 magic (0x36D76289)
 *   mb_info  — physical address of multiboot2 info struct
 */

/* Task 0: keyboard echo — reads keystrokes and prints them. */
static void
task_kbd(void)
{
    /* Enable interrupts. Tasks own their interrupt-enable state.
     * sti is called here (not in sched_start) so that the first ctx_switch
     * into this task lands on our own stack before the PIT can fire. */
    __asm__ volatile ("sti");
    for (;;) {
        char c = kbd_read();
        printk("%c", c);
    }
}

/* Task 1: heartbeat — exits after 500 ticks to allow make test to complete. */
static void
task_heartbeat(void)
{
    /* Enable interrupts — see task_kbd comment. */
    __asm__ volatile ("sti");
    for (;;) {
        if (arch_get_ticks() >= 500) {
            printk("[AEGIS] System halted.\n");
            /* FIXME: arch_debug_exit called from within a scheduled task.
             * Stack state is indeterminate. Acceptable for Phase 4 because
             * isa-debug-exit writes to an I/O port and QEMU exits immediately.
             * Phase 5+ must implement a clean kernel shutdown path. */
            arch_debug_exit(0x01);
        }
    }
}

void
kernel_main(uint32_t mb_magic, void *mb_info)
{
    (void)mb_magic;

    arch_init();            /* serial_init + vga_init                        */
    arch_mm_init(mb_info);  /* parse multiboot2 memory map                   */
    pmm_init();             /* bitmap allocator — [PMM] OK                   */
    vmm_init();             /* page tables, higher-half map — [VMM] OK       */
    cap_init();             /* capability stub — [CAP] OK                    */
    idt_init();             /* 48 interrupt gates — [IDT] OK                 */
    pic_init();             /* remap 8259A — [PIC] OK                        */
    pit_init();             /* 100 Hz timer — [PIT] OK                       */
    kbd_init();             /* PS/2 keyboard — [KBD] OK                      */
    sched_init();           /* init run queue (no tasks yet)                 */
    sched_spawn(task_kbd);
    sched_spawn(task_heartbeat);
    sched_start();          /* prints [SCHED] OK, switches into first task   */
    /* UNREACHABLE — sched_start() never returns */
    __builtin_unreachable();
}
