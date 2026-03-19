#include <stdint.h>
#include "arch.h"
#include "printk.h"
#include "cap.h"

/*
 * kernel_main — top-level kernel entry point.
 *
 * Called from boot.asm after long mode is established and a stack is set up.
 * Arguments follow the System V AMD64 ABI (set up in boot.asm).
 *
 * mb_magic: multiboot2 magic value (0x36D76289). Ignored in Phase 1;
 *           validated in PMM phase when we parse the memory map.
 *
 * mb_info:  physical address of the multiboot2 info structure.
 *           IMPORTANT: this is a physical address, not a virtual pointer.
 *           It equals the virtual address in Phase 1 due to identity mapping.
 *           The VMM phase must remap this before dereferencing it post-paging.
 */
void kernel_main(uint32_t mb_magic, void *mb_info)
{
    (void)mb_magic;
    (void)mb_info;

    arch_init();                           /* serial_init + vga_init */
    cap_init();                            /* [CAP] OK line */
    printk("[AEGIS] System halted.\n");
    arch_debug_exit(0x01);                 /* QEMU exits with code 3 */
    for (;;) {}                            /* if not running in QEMU */
}
