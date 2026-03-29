/* smp.c — SMP initialization, AP startup (INIT-SIPI-SIPI), per-CPU data */
#include "smp.h"
#include "arch.h"
#include "acpi.h"
#include "lapic.h"
#include "idt.h"
#include "gdt.h"
#include "tss.h"
#include "printk.h"
#include "kva.h"
#include "vmm.h"

#include <stdint.h>

/* ── Trampoline symbols (defined in ap_trampoline.asm) ──────────────── */
extern uint8_t  ap_trampoline_start[];
extern uint8_t  ap_trampoline_end[];
extern uint32_t ap_pml4;
extern uint64_t ap_entry_addr;
extern uint64_t ap_stacks[];

/* ── Globals ──────────────────────────────────────────────────────────── */
percpu_t g_percpu[MAX_CPUS];
uint32_t g_cpu_count = 1;
volatile uint8_t g_ap_online[MAX_CPUS];

/* Physical address where the trampoline is copied */
#define TRAMPOLINE_PHYS 0x8000

/* Number of 4KB pages per AP kernel stack */
#define AP_STACK_PAGES 4

void
smp_percpu_init_bsp(void)
{
    percpu_t *bsp = &g_percpu[0];
    __builtin_memset(bsp, 0, sizeof(*bsp));
    bsp->self     = bsp;
    bsp->cpu_id   = 0;
    bsp->lapic_id = 0;

    arch_set_gs_base((uint64_t)(uintptr_t)bsp);
    arch_write_kernel_gs_base((uint64_t)(uintptr_t)bsp);
}

/*
 * smp_start_aps — wake Application Processors via the INIT-SIPI-SIPI protocol.
 *
 * Sequence per AP:
 *   1. Copy trampoline to physical 0x8000 (identity-mapped)
 *   2. Fill data area (PML4 phys, ap_entry VA, per-CPU stack top)
 *   3. Send INIT IPI, wait ~20ms
 *   4. Send first SIPI (vector = 0x08 → phys 0x8000)
 *   5. Busy-wait ~200us
 *   6. Send second SIPI
 *   7. Poll g_ap_online[cpu_idx] for up to ~100ms
 */
void
smp_start_aps(void)
{
    uint32_t tramp_size = (uint32_t)(ap_trampoline_end - ap_trampoline_start);

    /* Offsets of data fields within the trampoline blob */
    uint32_t pml4_off   = (uint32_t)((uint8_t *)&ap_pml4      - ap_trampoline_start);
    uint32_t entry_off  = (uint32_t)((uint8_t *)&ap_entry_addr - ap_trampoline_start);
    uint32_t stacks_off = (uint32_t)((uint8_t *)ap_stacks      - ap_trampoline_start);

    /* 1. Copy trampoline to 0x8000 (within the 0-1GB identity map) */
    __builtin_memcpy((void *)(uintptr_t)TRAMPOLINE_PHYS,
                     ap_trampoline_start, tramp_size);

    /* 2. Fill shared data fields */
    *(volatile uint32_t *)(uintptr_t)(TRAMPOLINE_PHYS + pml4_off) =
        (uint32_t)vmm_get_master_pml4();
    *(volatile uint64_t *)(uintptr_t)(TRAMPOLINE_PHYS + entry_off) =
        (uint64_t)(uintptr_t)ap_entry;

    /* Use RDTSC-based busy-wait delays for INIT-SIPI-SIPI timing.
     * Do NOT enable interrupts here — ISR SWAPGS + iretq path crashes
     * before the scheduler is running (no valid task frame on the boot
     * stack). The LAPIC timer calibration needs PIT ticks but that
     * happens later in task_idle after sched_start. */

    /* 3. For each AP: allocate stack, fill percpu, send INIT-SIPI-SIPI */
    for (uint32_t i = 0; i < g_smp_cpu_count; i++) {
        uint8_t apic_id = g_smp_cpus[i].apic_id;

        /* Skip BSP */
        if (apic_id == g_bsp_apic_id)
            continue;

        /* Skip disabled CPUs from MADT */
        if (!g_smp_cpus[i].enabled)
            continue;

        /* Allocate per-AP kernel stack (4 pages = 16KB) */
        void *stack_base = kva_alloc_pages(AP_STACK_PAGES);
        if (!stack_base) {
            printk("[SMP] WARN: AP %u stack alloc failed\n", i);
            continue;
        }
        uint64_t stack_top = (uint64_t)(uintptr_t)stack_base +
                             AP_STACK_PAGES * 4096;

        /* Write stack top into trampoline data area (indexed by LAPIC ID) */
        *(volatile uint64_t *)(uintptr_t)(TRAMPOLINE_PHYS + stacks_off +
                                          apic_id * 8) = stack_top;

        /* Init percpu for this AP */
        percpu_t *p = &g_percpu[i];
        __builtin_memset(p, 0, sizeof(*p));
        p->self         = p;
        p->cpu_id       = (uint8_t)i;
        p->lapic_id     = apic_id;
        p->kernel_stack = stack_top;

        /* Clear online flag before sending IPIs */
        g_ap_online[i] = 0;

        /* Send INIT IPI */
        lapic_send_init(apic_id);

        /* Wait ~20ms via RDTSC busy-loop.
         * Assume ≥1 GHz TSC (true for all modern x86). 20ms ≈ 20M cycles. */
        {
            uint64_t start = arch_get_cycles();
            while (arch_get_cycles() - start < 20000000ULL)
                arch_pause();
        }

        /* Send first SIPI — vector 0x08 = physical 0x8000 */
        lapic_send_sipi(apic_id, 0x08);

        /* Busy-wait ~200us */
        {
            volatile uint32_t d = 100000;
            while (d--)
                ;
        }

        /* Send second SIPI */
        lapic_send_sipi(apic_id, 0x08);

        /* Poll for AP online, up to ~100ms (100M cycles at ≥1GHz) */
        {
            uint64_t start = arch_get_cycles();
            while (!g_ap_online[i] &&
                   (arch_get_cycles() - start) < 100000000ULL)
                arch_pause();
        }

        if (g_ap_online[i]) {
            g_cpu_count++;
        } else {
            printk("[SMP] WARN: AP %u (LAPIC %u) did not come online\n",
                   i, apic_id);
        }
    }

    printk("[SMP] OK: %u CPUs online\n", g_cpu_count);
}

/*
 * ap_entry — C entry point for Application Processors.
 *
 * Called from ap_trampoline.asm in 64-bit mode with:
 *   - RSP set to the per-CPU kernel stack top
 *   - CR3 loaded with the master PML4
 *   - Interrupts disabled (CLI in trampoline)
 *
 * Sets up per-CPU GS.base, enables the local APIC, loads the shared IDT,
 * signals the BSP, and enters the idle loop.
 */
void
ap_entry(void)
{
    /* Determine which CPU we are by LAPIC ID */
    uint8_t my_lapic = lapic_id();
    uint32_t my_idx = 0;
    for (uint32_t i = 0; i < g_smp_cpu_count; i++) {
        if (g_smp_cpus[i].apic_id == my_lapic) {
            my_idx = i;
            break;
        }
    }

    /* Set up per-CPU GS.base (both active and KERNEL_GS_BASE for swapgs) */
    percpu_t *me = &g_percpu[my_idx];
    arch_set_gs_base((uint64_t)(uintptr_t)me);
    arch_write_kernel_gs_base((uint64_t)(uintptr_t)me);

    /* Enable LAPIC on this AP (SVR + TPR, reuses BSP's MMIO mapping) */
    lapic_init_ap();

    /* Per-CPU GDT + TSS — without these, the AP has no user segments
     * (0x18/0x20), no TSS (RSP0 for ring-3 interrupts), and no IST
     * stack (double-fault). The trampoline's temporary GDT is insufficient. */
    arch_tss_init_ap(me->cpu_id);
    arch_gdt_init_ap(me->cpu_id, arch_tss_get_base_ap(me->cpu_id));

    /* Configure SYSCALL/SYSRET MSRs — per-CPU, needed for user tasks.
     * Call the shared init function; it prints [SYSCALL] OK which is fine
     * during AP bringup (boot oracle only runs on -machine pc with 1 CPU). */
    arch_syscall_init();

    /* Enable SMAP + SMEP on this AP (per-CPU CR4 bits).
     * Only set if BSP successfully enabled them (same CPU features). */
    if (arch_smap_enabled) {
        uint64_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1UL << 20) | (1UL << 21);  /* SMEP | SMAP */
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
    }

    /* Load the shared IDT (same gate table as BSP — at a fixed kernel VA) */
    arch_load_idt();

    /* LAPIC timer disabled on APs — causes #GP on Zen 2 bare metal.
     * TODO: debug vector 0x30 ISR gate on real hardware. */

    /* Signal BSP that we are online */
    g_ap_online[my_idx] = 1;

    /* Idle loop — APs wait here until the scheduler assigns work.
     * STI + HLT is atomic on x86: no interrupt window between them. */
    for (;;) {
        __asm__ volatile("sti; hlt; cli" ::: "memory");
    }
}
