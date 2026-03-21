#include "arch.h"
#include "printk.h"
#include "cap.h"
#include "pmm.h"
#include "vmm.h"
#include "kva.h"
#include "sched.h"
#include "../proc/proc.h"
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

    /* Spin until 500 ticks have elapsed. The scheduler preempts us on each
     * timer tick; we simply loop until the condition is met. */
    while (arch_get_ticks() < 500)
        ;

    printk("[AEGIS] System halted.\n");

    /* Request shutdown via pit_handler (ISR context, IF=0).
     * Calling arch_debug_exit directly from task context races with
     * QEMU 10's async isa-debug-exit: the CPU keeps running for several
     * ticks after the port write, allowing this task to re-enter printk
     * and emit a partial second line. Deferring to the ISR eliminates
     * the race — pit_handler runs with IF=0 and exits immediately. */
    arch_request_shutdown();

    /* Spin until the next timer tick fires and pit_handler exits QEMU. */
    for (;;)
        __asm__ volatile ("hlt");
}

void
kernel_main(uint32_t mb_magic, void *mb_info)
{
    (void)mb_magic;

    arch_init();            /* serial_init + vga_init                        */
    arch_mm_init(mb_info);  /* parse multiboot2 memory map                   */
    pmm_init();             /* bitmap allocator — [PMM] OK                   */
    vmm_init();             /* page tables, higher-half map — [VMM] OK       */
    kva_init();             /* kernel virtual allocator — [KVA] OK           */
    arch_set_master_pml4(vmm_get_master_pml4()); /* store master PML4 for ISR/SYSCALL */
    cap_init();             /* capability stub — [CAP] OK                    */
    idt_init();             /* 48 interrupt gates — [IDT] OK                 */
    pic_init();             /* remap 8259A — [PIC] OK                        */
    pit_init();             /* 100 Hz timer — [PIT] OK                       */
    kbd_init();             /* PS/2 keyboard — [KBD] OK                      */
    arch_gdt_init();        /* ring-3 GDT + TSS descriptors — [GDT] OK       */
    arch_tss_init();        /* TSS RSP0 for ring-3 → ring-0 transitions      */
    arch_syscall_init();    /* enable SYSCALL/SYSRET MSRs — [SYSCALL] OK     */
    sched_init();           /* init run queue (no tasks yet)                 */
    sched_spawn(task_kbd);
    sched_spawn(task_heartbeat);
    proc_spawn_init();      /* spawn init user process in ring 3             */
    /* All TCBs and stacks are in kva range at this point —
     * safe to remove the identity map. */
    vmm_teardown_identity(); /* pml4[0] = 0, CR3 reload — [VMM] OK          */
    sched_start();          /* prints [SCHED] OK, switches into first task   */
    /* UNREACHABLE — sched_start() never returns */
    __builtin_unreachable();
}
