/*
 * kernel/core/main.c — x86-64 kernel entry point.
 *
 * Arch isolation note (2026-04-12): This file is the x86-64 kernel
 * main. It pulls in x86-only subsystems (ACPI, LAPIC, IOAPIC, PCIe,
 * NVMe, xHCI, i8042) and is NOT built by the ARM64 Makefile
 * (kernel/arch/arm64/Makefile). The ARM64 port has its own entry
 * point at kernel/arch/arm64/main.c. Do not add arch-agnostic
 * initialization here — put it in a shared helper that both entries
 * can call, or guard it with #ifdef __x86_64__.
 */
#ifndef __x86_64__
#error "kernel/core/main.c is x86-64 only; arm64 uses kernel/arch/arm64/main.c"
#endif

#include "arch.h"
#include "printk.h"
#include "cap.h"
#include "pmm.h"
#include "vmm.h"
#include "kva.h"
#include "sched.h"
#include "proc.h"
#include "vfs.h"
#include "console.h"
#include "acpi.h"
#include "smp.h"
#include "lapic.h"
#include "ioapic.h"
#include "pcie.h"
#include "nvme.h"
#include "ext2.h"
#include "gpt.h"
#include "xhci.h"
#include "ps2_mouse.h"
#include "virtio_net.h"
#include "rtl8169.h"
#include "fb.h"
#include "ramdisk.h"
#include "ip.h"
#include "blkdev.h"
#include "random.h"
#include <stdint.h>

void poll_test(void);

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
    arch_mm_init(mb_info);  /* parse multiboot2 memory map + cmdline         */
    /* Parse boot mode + quiet flag from kernel cmdline.
     * boot=text  → text console, no splash, printk writes to FB normally.
     * boot=graphical quiet → splash, printk suppressed on FB. */
    int text_mode = 0;
    {
        const char *cmdline = arch_get_cmdline();
        const char *p = cmdline;
        while (*p) {
            if (p[0]=='b' && p[1]=='o' && p[2]=='o' && p[3]=='t' &&
                p[4]=='=' && p[5]=='t' && p[6]=='e' && p[7]=='x' && p[8]=='t')
                { text_mode = 1; break; }
            p++;
        }
        if (!text_mode) {
            p = cmdline;
            while (*p) {
                if (p[0]=='q' && p[1]=='u' && p[2]=='i' && p[3]=='e' && p[4]=='t')
                    { printk_set_quiet(1); break; }
                p++;
            }
        }
        if (cmdline[0])
            printk("[CMDLINE] OK: %s\n", cmdline);
        else
            printk("[CMDLINE] OK: (none)\n");
    }
    pmm_init();             /* bitmap allocator — [PMM] OK                   */
    vmm_init();             /* page tables, higher-half map — [VMM] OK       */
    kva_init();             /* kernel virtual allocator — [KVA] OK           */
    arch_set_master_pml4(vmm_get_master_pml4()); /* store master PML4 for ISR/SYSCALL */
    fb_init();              /* linear framebuffer — [FB] OK or silent        */
    if (!text_mode)
        fb_boot_splash();   /* draw logo immediately (graphical boot only)   */
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
    /* Flush i8042 output buffer after PIC→IOAPIC transition.
     * Stale scancodes from BIOS/GRUB can hold IRQ1 asserted on the
     * i8042, preventing new keyboard interrupts until the buffer is
     * drained.  This fixes intermittent "no keyboard on boot" on bare
     * metal (2/3 boots affected on ThinkPad X13 Zen 2). */
    while (inb(0x64) & 0x01)
        (void)inb(0x60);
    pcie_init();            /* enumerate PCIe devices — [PCIE] OK            */
    fb_check_amd();         /* warn if AMD GPU present but no UEFI fb tag    */
    nvme_init();            /* NVMe block device — [NVME] OK or silent skip  */
    gpt_scan("nvme0");      /* GPT partitions — [GPT] OK or silent (no NVMe) */
    /* Mount ext2 root filesystem.
     * If a ramdisk module is present (live USB/CDROM boot), ALWAYS use it —
     * never silently pick up an NVMe install which may be stale or broken.
     * Only mount NVMe when no ramdisk exists (installed system booting
     * from its own disk without GRUB modules). */
    if (blkdev_get("ramdisk0")) {
        ext2_mount("ramdisk0");
    } else if (ext2_mount("nvme0p1") != 0) {
        printk("[VFS] WARN: no ramdisk and no Aegis root on NVMe — running from initrd only\n");
    }
    cap_policy_load();      /* load /etc/aegis/caps.d/ — must be after ext2  */
    poll_test();            /* VFS .poll self-test — [POLL] OK               */
    xhci_init();            /* xHCI USB host — [XHCI] OK or silent skip     */
    virtio_net_init();      /* virtio-net NIC — [NET] OK or silent skip      */
    rtl8169_init();         /* RTL8168/8169 NIC — [NET] OK or silent skip   */
    net_init();             /* Phase 25: protocol stack init + ICMP self-test ping */
    smp_start_aps();        /* wake APs via INIT-SIPI-SIPI — [SMP] OK       */
    sched_init();           /* init run queue (no tasks yet)                 */
    sched_spawn(task_idle);
    proc_spawn_init();      /* spawn init user process in ring 3             */
    /* All TCBs and stacks are in kva range at this point —
     * safe to remove the identity map. */
    vmm_teardown_identity(); /* pml4[0] = 0, CR3 reload — [VMM] OK          */
    fb_boot_splash_end();   /* clear splash, unlock FB — all kernel init done */
    /* LAPIC timer starts from task_idle (after sched_start ctx_switches).
     * Starting it here would fire vector 0x30 before the scheduler is ready. */
    sched_start();          /* prints [SCHED] OK, switches into first task   */
    /* UNREACHABLE — sched_start() never returns */
    __builtin_unreachable();
}
