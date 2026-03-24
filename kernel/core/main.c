#include "arch.h"
#include "printk.h"
#include "cap.h"
#include "pmm.h"
#include "vmm.h"
#include "kva.h"
#include "sched.h"
#include "../proc/proc.h"
#include "vfs.h"
#include "console.h"
#include "acpi.h"
#include "pcie.h"
#include "nvme.h"
#include "../fs/ext2.h"
#include "../fs/gpt.h"
#include "../drivers/xhci.h"
#include "../drivers/virtio_net.h"
#include "../net/ip.h"
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

/* Task 0: idle — enables interrupts and halts until next tick.
 * Never exits. Shutdown is triggered by sched_exit when the last user
 * process calls sys_exit. */
static void
task_idle(void)
{
    __asm__ volatile ("sti");
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
    arch_smap_init();       /* SMAP detect + enable — [SMAP] OK/WARN         */
    arch_smep_init();       /* SMEP detect + enable — [SMEP] OK/WARN         */
    arch_sse_init();        /* enable SSE for user mode (CR0/CR4 bits)       */
    vfs_init();             /* [VFS] OK + [INITRD] OK                        */
    console_init();         /* register stdout device (silent)               */
    acpi_init();            /* parse MCFG+MADT — [ACPI] OK                   */
    pcie_init();            /* enumerate PCIe devices — [PCIE] OK            */
    nvme_init();            /* NVMe block device — [NVME] OK or silent skip  */
    gpt_scan("nvme0");      /* GPT partitions — [GPT] OK or silent (no NVMe) */
    ext2_mount("nvme0p1");  /* mount partition 1 — [EXT2] OK or silent (-1)  */
    xhci_init();            /* xHCI USB host — [XHCI] OK or silent skip     */
    virtio_net_init();      /* virtio-net NIC — [NET] OK or silent skip      */
    net_init();             /* Phase 25: protocol stack init + ICMP self-test ping */
    sched_init();           /* init run queue (no tasks yet)                 */
    sched_spawn(task_idle);
    proc_spawn_init();      /* spawn init user process in ring 3             */
    /* All TCBs and stacks are in kva range at this point —
     * safe to remove the identity map. */
    vmm_teardown_identity(); /* pml4[0] = 0, CR3 reload — [VMM] OK          */
    sched_start();          /* prints [SCHED] OK, switches into first task   */
    /* UNREACHABLE — sched_start() never returns */
    __builtin_unreachable();
}
