#include "arch.h"
#include "serial.h"
#include "vga.h"

/*
 * arch_init — initialize all x86_64 early subsystems.
 *
 * Called once from kernel_main before any other subsystem.
 * Order matters: serial must be up before vga_init (vga_init calls serial).
 * Clobbers: nothing directly (subsystems manage their own state).
 */
void arch_init(void)
{
    serial_init();
    vga_init();
}
