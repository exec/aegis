#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "kbd.h"
#include "ps2_mouse.h"
#include "acpi.h"
#include "lapic.h"
#include "ioapic.h"
#include "tlb.h"
#include "printk.h"
#include "arch.h"

/* 256 entries: full IDT covering all x86-64 interrupt vectors */
static aegis_idt_gate_t s_idt[256];

/* Prototypes for the 48 ISR stubs defined in isr.asm */
extern void *isr_stubs[48];

/* LAPIC timer vector stub (vector 0x30) defined in isr.asm */
extern void *isr_stub_lapic_timer;

/* TLB shootdown IPI vector stub (vector 0xFE) defined in isr.asm */
extern void *isr_stub_tlb_shootdown;

/* LAPIC spurious vector stub (vector 0xFF) defined in isr.asm */
extern void *isr_stub_spurious;

static void
idt_gate_set(uint8_t vec, void *handler)
{
    uint64_t addr = (uint64_t)handler;
    s_idt[vec].offset_lo  = addr & 0xFFFF;
    s_idt[vec].selector   = ARCH_KERNEL_CS;  /* kernel code segment */
    s_idt[vec].ist        = 0;
    s_idt[vec].type_attr  = 0x8E;           /* present, DPL=0, interrupt gate */
    s_idt[vec].offset_mid = (addr >> 16) & 0xFFFF;
    s_idt[vec].offset_hi  = (addr >> 32) & 0xFFFFFFFF;
    s_idt[vec].zero       = 0;
}

void
idt_init(void)
{
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr = {
        .limit = sizeof(s_idt) - 1,
        .base  = (uint64_t)s_idt
    };

    for (int i = 0; i < 48; i++)
        idt_gate_set((uint8_t)i, isr_stubs[i]);

    /* Vector 0x30 — LAPIC timer interrupt (periodic, ~100Hz). */
    idt_gate_set(0x30, isr_stub_lapic_timer);

    /* Vector 0xFE — TLB shootdown IPI from tlb_shootdown().  The handler
     * does its own LAPIC EOI; isr_dispatch skips the normal EOI path. */
    idt_gate_set(0xFE, isr_stub_tlb_shootdown);

    /* Vector 0xFF — LAPIC spurious interrupt.  Per Intel spec, no EOI must
     * be sent for spurious interrupts; isr_dispatch returns immediately. */
    idt_gate_set(0xFF, isr_stub_spurious);

    /* #DF (double fault, vector 8) uses IST1 — dedicated stack prevents
     * triple fault when #DF occurs due to stack overflow or RSP corruption.
     * IST field value 1 in the gate maps to tss.ist[0] (Intel 1-indexed,
     * C array 0-indexed). */
    s_idt[8].ist = 1;

    __asm__ volatile ("lidt %0" : : "m"(idtr));
    printk("[IDT] OK: 256 vectors installed\n");
}

void
arch_load_idt(void)
{
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr = {
        .limit = sizeof(s_idt) - 1,
        .base  = (uint64_t)s_idt
    };
    __asm__ volatile("lidt %0" : : "m"(idtr));
}

/* Walk the RBP frame-pointer chain and print return addresses.
 * Only valid for kernel-mode exceptions (CS=ARCH_KERNEL_CS).
 * At -O0 with -fno-omit-frame-pointer, every frame pushes RBP.
 * Each frame: [rbp+0] = saved previous RBP, [rbp+8] = return address.
 * Resolve addresses with: make sym ADDR=0x<addr>
 * Limit of 16 frames keeps us well within a single 4KB kernel stack page. */
static void
panic_backtrace(uint64_t rbp)
{
    printk("[PANIC] backtrace (resolve: make sym ADDR=0x<addr>):\n");
    for (int i = 0; i < 16; i++) {
        /* RBP must be 8-byte aligned and in the higher-half kernel VA range */
        if (rbp < 0xFFFFFFFF80000000ULL || (rbp & 7ULL))
            break;
        uint64_t retaddr = ((uint64_t *)rbp)[1];
        if (retaddr < 0xFFFFFFFF80000000ULL || retaddr == 0)
            break;
        printk("[PANIC]   [%u] 0x%lx\n", i, retaddr);
        rbp = ((uint64_t *)rbp)[0];
    }
}

void __attribute__((used))
isr_dispatch(cpu_state_t *s)
{
    if (s->vector < 32) {
        /* CPU exception: print and halt */
        printk("[PANIC] exception %lu at RIP=0x%lx error=0x%lx CS=0x%lx\n",
               s->vector, s->rip, s->error_code, s->cs);
        /* For #PF: print CR2 and key registers */
        if (s->vector == 14) {
            uint64_t cr2, fsbase;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            /* IA32_FS_BASE MSR = 0xC0000100 */
            uint32_t fs_lo, fs_hi;
            __asm__ volatile("rdmsr" : "=a"(fs_lo), "=d"(fs_hi) : "c"(0xC0000100U));
            fsbase = ((uint64_t)fs_hi << 32) | fs_lo;
            printk("[PANIC] #PF CR2=0x%lx rax=0x%lx rbx=0x%lx\n",
                   cr2, s->rax, s->rbx);
            printk("[PANIC] #PF r12=0x%lx r13=0x%lx r14=0x%lx\n",
                   s->r12, s->r13, s->r14);
            printk("[PANIC] #PF rsp=0x%lx ss=0x%lx fs_base=0x%lx\n",
                   s->rsp, s->ss, fsbase);
        }
        /* For #GP at iretq: the original iretq frame overlaps cpu_state_t.rsp/ss.
         * Print all 5 slots of the frame so we can see what iretq was trying to load. */
        if (s->vector == 13) {
            uint64_t *f = (uint64_t *)&s->rsp;
            printk("[PANIC] #GP iretq frame: RIP=%lx CS=%lx FL=%lx RSP=%lx SS=%lx\n",
                   f[0], f[1], f[2], f[3], f[4]);
        }
        /* Print kernel backtrace for kernel-mode faults.
         * For ring-3 faults (CS=ARCH_USER_CS) rbp is a user-space pointer — skip. */
        if (s->cs == ARCH_KERNEL_CS)
            panic_backtrace(s->rbp);
        for (;;) {}
    } else if (s->vector == 0xFE) {
        /* TLB shootdown IPI — handler does its own LAPIC EOI. */
        tlb_shootdown_handler();
        return;
    } else if (s->vector == 0xFF) {
        /* LAPIC spurious interrupt — per Intel spec, do NOT send EOI. */
        return;
    } else if (s->vector == 0x30) {
        /* LAPIC timer interrupt — send EOI before handler because
         * sched_tick may ctx_switch away, leaving the EOI unsent.
         * Edge-triggered periodic LAPIC timer: EOI-first is safe. */
        lapic_eoi();
        lapic_timer_handler();
    } else if (s->vector >= 0x20 && s->vector < 0x30) {
        /* Hardware IRQ: send EOI BEFORE the handler.
         * pit_handler calls sched_tick which calls ctx_switch — if we
         * switch away before EOI, the outgoing task carries the EOI
         * obligation and IRQ0 goes dark until that task is rescheduled. */
        uint8_t irq = (uint8_t)(s->vector - 0x20);

        if (lapic_active()) {
            lapic_eoi();
        } else {
            /* Guard against spurious IRQ7 (PIC1) and IRQ15 (PIC2 cascade).
             * Per 8259A spec: do NOT send EOI for spurious interrupts — doing
             * so clears the ISR bit of a real in-service interrupt.
             * For spurious IRQ15 (PIC2), PIC1 still received the cascade
             * interrupt on IRQ2, so we must EOI PIC1 only. */
            if ((irq == 7 || irq == 15) && !pic_irq_is_real(irq)) {
                if (irq == 15)
                    outb(0x20, 0x20);   /* spurious IRQ15: EOI to PIC1 only */
                /* No EOI to PIC2 for spurious IRQ15; no EOI at all for spurious IRQ7. */
            } else {
                pic_send_eoi(irq);
            }
        }

        if      (s->vector == 0x20) { pit_handler(); }
        else if (s->vector == 0x21) { kbd_handler(); }
        else if (s->vector == 0x2C) { ps2_mouse_handler(); }
        else if (irq == acpi_get_sci_irq() && irq != 0) { acpi_sci_handler(); }
    }

    /* Sanity-check the iretq frame for ring-3 interrupts.
     * For a ring-3 interrupt (cs=ARCH_USER_CS), cpu_state_t.rsp=user_rsp and
     * cpu_state_t.ss must point to the user data descriptor (GDT index 3).
     *
     * AMD CPUs in 64-bit mode may strip SS RPL bits after loading, pushing
     * 0x18 (RPL=0) instead of 0x1B (RPL=3) on interrupt entry — both are
     * valid since SS is unused for addressing in 64-bit long mode.
     * Accept any RPL variant of the user data selector: (ss & ~3) == 0x18.
     * Reject kernel selectors (ARCH_KERNEL_CS, ARCH_KERNEL_DS) or user code (0x20). */
    if (s->cs == ARCH_USER_CS && (s->ss & ~(uint64_t)3) != (ARCH_USER_DS & ~(uint64_t)3)) {
        printk("[PANIC] corrupt ring-3 iretq frame vec=%lu rip=0x%lx rsp=0x%lx ss=0x%lx\n",
               s->vector, s->rip, s->rsp, s->ss);
        for (;;) {}
    }

    /* Normalize SS RPL=3 for iretq back to ring-3.
     * AMD CPUs in 64-bit mode strip SS RPL bits on ring-3 interrupt entry,
     * pushing SS=0x18 (RPL=0) instead of SS=ARCH_USER_DS (RPL=3).  iretq to
     * ring-3 (CPL=3) requires SS.RPL == CPL=3; without normalization the
     * iretq faults #GP(SS) on AMD.  Force RPL=3 unconditionally on the return
     * path — harmless on Intel (which already pushes RPL=3), required on AMD. */
    if (s->cs == ARCH_USER_CS)
        s->ss |= 3;
}
