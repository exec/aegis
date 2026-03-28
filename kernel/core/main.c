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
#include "smp.h"
#include "lapic.h"
#include "ioapic.h"
#include "pcie.h"
#include "nvme.h"
#include "../fs/ext2.h"
#include "../fs/gpt.h"
#include "../drivers/xhci.h"
#include "ps2_mouse.h"
#include "../drivers/virtio_net.h"
#include "../drivers/fb.h"
#include "../drivers/ramdisk.h"
#include "../net/ip.h"
#include "../fs/blkdev.h"
#include "random.h"
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
    lapic_timer_init();
    arch_enable_irq();
    for (;;)
        arch_halt();
}

void
kernel_main(uint32_t mb_magic, void *mb_info)
{
    (void)mb_magic;

    arch_init();            /* serial_init + vga_init                        */
    arch_pat_init();        /* PAT MSR: PA1=WC for framebuffer mapping       */
    arch_mm_init(mb_info);  /* parse multiboot2 memory map                   */
    pmm_init();             /* bitmap allocator — [PMM] OK                   */
    vmm_init();             /* page tables, higher-half map — [VMM] OK       */
    kva_init();             /* kernel virtual allocator — [KVA] OK           */
    arch_set_master_pml4(vmm_get_master_pml4()); /* store master PML4 for ISR/SYSCALL */
    fb_init();              /* linear framebuffer — [FB] OK or silent        */
    cap_init();             /* capability stub — [CAP] OK                    */
    smp_percpu_init_bsp();  /* per-CPU data — [SMP] OK                       */
    idt_init();             /* 48 interrupt gates — [IDT] OK                 */
    pic_init();             /* remap 8259A — [PIC] OK                        */
    pit_init();             /* 100 Hz timer — [PIT] OK                       */
    kbd_init();             /* PS/2 keyboard — [KBD] OK                      */
    ps2_mouse_init();       /* PS/2 mouse — [MOUSE] OK                       */
    arch_gdt_init();        /* ring-3 GDT + TSS descriptors — [GDT] OK       */
    arch_tss_init();        /* TSS RSP0 for ring-3 → ring-0 transitions      */
    arch_syscall_init();    /* enable SYSCALL/SYSRET MSRs — [SYSCALL] OK     */
    arch_smap_init();       /* SMAP detect + enable — [SMAP] OK/WARN         */
    arch_smep_init();       /* SMEP detect + enable — [SMEP] OK/WARN         */
    arch_sse_init();        /* enable SSE for user mode (CR0/CR4 bits)       */
    random_init();          /* ChaCha20 CSPRNG — [RNG] OK                    */

    /* Map GRUB modules into KVA as RAM blkdevs */
    {
        uint64_t mod_phys = 0, mod_size = 0;
        arch_get_module(&mod_phys, &mod_size);
        ramdisk_init(mod_phys, mod_size);    /* ramdisk0 = rootfs */
    }
    {
        uint64_t mod2_phys = 0, mod2_size = 0;
        arch_get_module2(&mod2_phys, &mod2_size);
        ramdisk_init2(mod2_phys, mod2_size); /* ramdisk1 = ESP image */
    }

    vfs_init();             /* [VFS] OK + [INITRD] OK                        */
    console_init();         /* register stdout device (silent)               */
    acpi_init();            /* parse MCFG+MADT — [ACPI] OK                   */
    lapic_init();           /* Local APIC — [LAPIC] OK or silent skip        */
    ioapic_init();          /* I/O APIC — [IOAPIC] OK or silent skip         */
    pcie_init();            /* enumerate PCIe devices — [PCIE] OK            */
    fb_check_amd();         /* warn if AMD GPU present but no UEFI fb tag    */
    nvme_init();            /* NVMe block device — [NVME] OK or silent skip  */
    gpt_scan("nvme0");      /* GPT partitions — [GPT] OK or silent (no NVMe) */
    /* Mount ext2: prefer NVMe (installed system), fall back to ramdisk (live) */
    if (ext2_mount("nvme0p1") != 0) {
        if (blkdev_get("nvme0"))
            printk("[VFS] WARN: NVMe present but no Aegis root partition — falling back to ramdisk (VOLATILE)\n");
        ext2_mount("ramdisk0");
    }
    xhci_init();            /* xHCI USB host — [XHCI] OK or silent skip     */
    virtio_net_init();      /* virtio-net NIC — [NET] OK or silent skip      */
    net_init();             /* Phase 25: protocol stack init + ICMP self-test ping */
    smp_start_aps();        /* wake APs via INIT-SIPI-SIPI — [SMP] OK       */
    sched_init();           /* init run queue (no tasks yet)                 */
    sched_spawn(task_idle);
    proc_spawn_init();      /* spawn init user process in ring 3             */
    /* All TCBs and stacks are in kva range at this point —
     * safe to remove the identity map. */
    vmm_teardown_identity(); /* pml4[0] = 0, CR3 reload — [VMM] OK          */
    /* LAPIC timer starts from task_idle (after sched_start ctx_switches).
     * Starting it here would fire vector 0x30 before the scheduler is ready. */
    sched_start();          /* prints [SCHED] OK, switches into first task   */
    /* UNREACHABLE — sched_start() never returns */
    __builtin_unreachable();
}
