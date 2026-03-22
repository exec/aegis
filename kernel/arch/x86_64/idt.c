#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "kbd.h"
#include "printk.h"

/* 48 entries: vectors 0x00-0x1F (CPU exceptions) + 0x20-0x2F (IRQs) */
static aegis_idt_gate_t s_idt[48];

/* Prototypes for the 48 ISR stubs defined in isr.asm */
extern void *isr_stubs[48];

static void
idt_gate_set(uint8_t vec, void *handler)
{
    uint64_t addr = (uint64_t)handler;
    s_idt[vec].offset_lo  = addr & 0xFFFF;
    s_idt[vec].selector   = 0x08;           /* kernel code segment */
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

    __asm__ volatile ("lidt %0" : : "m"(idtr));
    printk("[IDT] OK: 48 vectors installed\n");
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
        for (;;) {}
    } else if (s->vector < 0x30) {
        /* Hardware IRQ: send EOI BEFORE the handler.
         * pit_handler calls sched_tick which calls ctx_switch — if we
         * switch away before EOI, the outgoing task carries the EOI
         * obligation and IRQ0 goes dark until that task is rescheduled. */
        uint8_t irq = (uint8_t)(s->vector - 0x20);
        pic_send_eoi(irq);
        if      (s->vector == 0x20) { pit_handler(); }
        else if (s->vector == 0x21) { kbd_handler(); }

        /* Sanity-check the iretq frame for ring-3 interrupts.
         * For a ring-3 interrupt (cs=0x23), cpu_state_t.rsp=user_rsp and
         * cpu_state_t.ss=user_SS which must be 0x1B.  If the frame is
         * corrupted we would get a silent #GP at iretq; catch it here. */
        if (s->cs == 0x23 && s->ss != 0x1B) {
            printk("[PANIC] corrupt ring-3 iretq frame vec=%lu ss=0x%lx rsp=0x%lx\n",
                   s->vector, s->ss, s->rsp);
            for (;;) {}
        }
    }
    /* vectors >= 0x30: not installed, ignored */
}
