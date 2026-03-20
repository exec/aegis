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
idt_set_gate(uint8_t vec, void *handler)
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
        idt_set_gate((uint8_t)i, isr_stubs[i]);

    __asm__ volatile ("lidt %0" : : "m"(idtr));
    printk("[IDT] OK: 48 vectors installed\n");
}

void __attribute__((used))
isr_dispatch(cpu_state_t *s)
{
    if (s->vector < 32) {
        /* CPU exception: print and halt */
        printk("[PANIC] exception %lu at RIP=0x%lx error=0x%lx\n",
               s->vector, s->rip, s->error_code);
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
    }
    /* vectors >= 0x30: not installed, ignored */
}
