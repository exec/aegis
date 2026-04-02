# Phase 4: IDT + PIC + PIT + PS/2 Keyboard + Preemptive Scheduler — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a preemptive round-robin scheduler driven by a 100 Hz PIT timer, a PS/2 keyboard driver, and an interactive VGA terminal with cursor and scrolling.

**Architecture:** IDT installs 48 interrupt gates (32 CPU exceptions + 16 remapped IRQs). PIC remaps 8259A so IRQs land at vectors 0x20–0x2F. PIT fires at 100 Hz, triggering `sched_tick()` which calls `ctx_switch()` to preempt tasks. Two kernel tasks run concurrently: keyboard echo and heartbeat. `make test` exits via the heartbeat task after 500 ticks (~5 seconds).

**Tech Stack:** C (kernel), NASM (ISR stubs + context switch), x86-64, 8259A PIC, 8253 PIT, PS/2 keyboard controller.

**Spec:** `docs/superpowers/specs/2026-03-20-phase4-scheduler-design.md`

---

## File Map

| File | Action | Purpose |
|------|--------|---------|
| `tests/expected/boot.txt` | Modify | Add 5 new subsystem lines (RED) |
| `Makefile` | Modify | Add ARCH_ASMS, SCHED_SRCS, -Ikernel/sched, outb/inb in arch.h |
| `kernel/arch/x86_64/arch.h` | Modify | Add outb/inb, new declarations for all Phase 4 functions |
| `kernel/arch/x86_64/pic.h` | Create | PIC interface |
| `kernel/arch/x86_64/pic.c` | Create | 8259A remap, EOI, mask |
| `kernel/arch/x86_64/idt.h` | Create | IDT gate struct, cpu_state_t, idt_init |
| `kernel/arch/x86_64/idt.c` | Create | IDT table, isr_dispatch |
| `kernel/arch/x86_64/isr.asm` | Create | 48 ISR entry stubs + common stub |
| `kernel/arch/x86_64/pit.h` | Create | PIT interface, arch_get_ticks |
| `kernel/arch/x86_64/pit.c` | Create | PIT at 100 Hz, tick counter |
| `kernel/arch/x86_64/kbd.h` | Create | Keyboard interface |
| `kernel/arch/x86_64/kbd.c` | Create | PS/2 scancode→ASCII, ring buffer |
| `kernel/arch/x86_64/vga.h` | Modify | Add vga_putchar declaration |
| `kernel/arch/x86_64/vga.c` | Modify | vga_putchar with cursor, scroll, pushfq/popfq |
| `kernel/arch/x86_64/ctx_switch.asm` | Create | Callee-saved register context switch |
| `kernel/sched/sched.h` | Create | TCB, scheduler interface |
| `kernel/sched/sched.c` | Create | Run queue, spawn, tick, _Static_assert |
| `kernel/core/main.c` | Modify | Wire new inits, spawn tasks, sched_start |
| `.claude/CLAUDE.md` | Modify | Update build status |

---

## Task 1: RED — Update boot.txt

**Files:**
- Modify: `tests/expected/boot.txt`

- [ ] **Step 1: Read the current boot.txt**

```bash
cat tests/expected/boot.txt
```

Expected current content:
```
[SERIAL] OK: COM1 initialized at 115200 baud
[VGA] OK: text mode 80x25
[PMM] OK: 127MB usable across 2 regions
[VMM] OK: kernel mapped to 0xFFFFFFFF80000000
[CAP] OK: capability subsystem reserved
[AEGIS] System halted.
```

- [ ] **Step 2: Add 5 new lines before `[AEGIS]`**

The new file must contain exactly:
```
[SERIAL] OK: COM1 initialized at 115200 baud
[VGA] OK: text mode 80x25
[PMM] OK: 127MB usable across 2 regions
[VMM] OK: kernel mapped to 0xFFFFFFFF80000000
[CAP] OK: capability subsystem reserved
[IDT] OK: 48 vectors installed
[PIC] OK: IRQ0-15 remapped to vectors 0x20-0x2F
[PIT] OK: timer at 100 Hz
[KBD] OK: PS/2 keyboard ready
[SCHED] OK: scheduler started, 2 tasks
[AEGIS] System halted.
```

No trailing spaces. Single newline at end of file.

- [ ] **Step 3: Verify make test fails (RED)**

```bash
make test
```

Expected: exits non-zero. The diff will show 5 missing lines (`< [IDT]`, `< [PIC]`, etc.).

- [ ] **Step 4: Commit**

```bash
git add tests/expected/boot.txt
git commit -m "test(phase4): add expected output for IDT/PIC/PIT/KBD/SCHED (RED)"
```

---

## Task 2: Makefile + arch.h infrastructure

**Files:**
- Modify: `Makefile`
- Modify: `kernel/arch/x86_64/arch.h`

- [ ] **Step 1: Read the current Makefile and arch.h**

```bash
cat Makefile
cat kernel/arch/x86_64/arch.h
```

- [ ] **Step 2: Update the Makefile**

Make the following changes to `Makefile`:

**Add `-Ikernel/sched` to CFLAGS** (after `-Ikernel/mm`):
```makefile
    -Ikernel/mm \
    -Ikernel/sched
```

**Add new C files to ARCH_SRCS** (after `vga.c`):
```makefile
    kernel/arch/x86_64/idt.c \
    kernel/arch/x86_64/pic.c \
    kernel/arch/x86_64/pit.c \
    kernel/arch/x86_64/kbd.c
```

**Add after the `BOOT_SRC` line:**
```makefile
ARCH_ASMS = \
    kernel/arch/x86_64/isr.asm \
    kernel/arch/x86_64/ctx_switch.asm

SCHED_SRCS = kernel/sched/sched.c
```

**Add after the existing `BOOT_OBJ` line:**
```makefile
ARCH_ASM_OBJS = $(patsubst kernel/%.asm,$(BUILD)/%.o,$(ARCH_ASMS))
SCHED_OBJS    = $(patsubst kernel/%.c,$(BUILD)/%.o,$(SCHED_SRCS))
```

**Update `ALL_OBJS`** to include the new object groups:
```makefile
ALL_OBJS = $(BOOT_OBJ) $(ARCH_OBJS) $(ARCH_ASM_OBJS) $(CORE_OBJS) $(MM_OBJS) $(SCHED_OBJS)
```

**Add a pattern rule for ARCH_ASMS** (after the existing `$(BOOT_OBJ)` rule):
```makefile
$(BUILD)/arch/x86_64/%.o: kernel/arch/x86_64/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@
```

- [ ] **Step 3: Add outb/inb to arch.h**

Add these two static inline functions to `arch.h` in the "Architecture-specific I/O" section (add before the physical memory interface section):

```c
/* -------------------------------------------------------------------------
 * Arch-specific port I/O primitives
 * Used by serial, PIC, PIT, VGA hardware cursor — not for use in kernel/core/.
 * ------------------------------------------------------------------------- */
static inline void
outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t
inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}
```

- [ ] **Step 4: Verify the build still compiles**

```bash
make 2>&1 | head -20
```

Expected: existing files compile without error. The link will fail with undefined symbols for the new files (expected — we haven't written them yet).

- [ ] **Step 5: Commit**

```bash
git add Makefile kernel/arch/x86_64/arch.h
git commit -m "build: add Phase 4 source groups, -Ikernel/sched, outb/inb to arch.h"
```

---

## Task 3: PIC — 8259A remap and EOI

**Files:**
- Create: `kernel/arch/x86_64/pic.h`
- Create: `kernel/arch/x86_64/pic.c`

- [ ] **Step 1: Create `pic.h`**

```c
#ifndef AEGIS_PIC_H
#define AEGIS_PIC_H

#include <stdint.h>

/* Initialize the 8259A dual PIC.
 * Remaps IRQ0-7 to vectors 0x20-0x27, IRQ8-15 to 0x28-0x2F.
 * Masks all IRQs after remapping (drivers unmask their own IRQ). */
void pic_init(void);

/* Send End-Of-Interrupt to the PIC.
 * For IRQ >= 8, sends EOI to both slave and master. */
void pic_send_eoi(uint8_t irq);

/* Unmask (enable) a single IRQ line (0-15). */
void pic_unmask(uint8_t irq);

/* Mask (disable) a single IRQ line (0-15). */
void pic_mask(uint8_t irq);

#endif /* AEGIS_PIC_H */
```

- [ ] **Step 2: Create `pic.c`**

```c
#include "pic.h"
#include "arch.h"
#include "printk.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define PIC_EOI   0x20

/* ICW1: start init sequence, ICW4 needed */
#define ICW1_INIT 0x11
/* ICW4: 8086 mode */
#define ICW4_8086 0x01

/* Small delay via port 0x80 (POST diagnostic port — always safe to write) */
static void
io_wait(void)
{
    outb(0x80, 0);
}

void
pic_init(void)
{
    /* Start init sequence (cascade mode) */
    outb(PIC1_CMD,  ICW1_INIT); io_wait();
    outb(PIC2_CMD,  ICW1_INIT); io_wait();

    /* ICW2: vector offsets */
    outb(PIC1_DATA, 0x20);      io_wait(); /* master: IRQ0-7 → 0x20-0x27 */
    outb(PIC2_DATA, 0x28);      io_wait(); /* slave:  IRQ8-15 → 0x28-0x2F */

    /* ICW3: cascade wiring */
    outb(PIC1_DATA, 0x04);      io_wait(); /* master: slave on IRQ2 (bit 2) */
    outb(PIC2_DATA, 0x02);      io_wait(); /* slave: cascade identity = 2 */

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* Mask all IRQs — drivers call pic_unmask() when ready */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    printk("[PIC] OK: IRQ0-15 remapped to vectors 0x20-0x2F\n");
}

void
pic_send_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void
pic_unmask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = irq & 7;
    outb(port, inb(port) & ~(1 << bit));
}

void
pic_mask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = irq & 7;
    outb(port, inb(port) | (1 << bit));
}
```

- [ ] **Step 3: Verify pic.c compiles**

```bash
make 2>&1 | grep -E "pic|error" | head -20
```

Expected: `pic.c` compiles cleanly. Link still fails (other new files missing).

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/pic.h kernel/arch/x86_64/pic.c
git commit -m "arch: add 8259A PIC driver (pic_init, pic_send_eoi, pic_unmask)"
```

---

## Task 4: IDT — gate descriptors and dispatch

**Files:**
- Create: `kernel/arch/x86_64/idt.h`
- Create: `kernel/arch/x86_64/idt.c`

- [ ] **Step 1: Create `idt.h`**

```c
#ifndef AEGIS_IDT_H
#define AEGIS_IDT_H

#include <stdint.h>

/* x86-64 IDT gate descriptor (16 bytes, packed) */
typedef struct {
    uint16_t offset_lo;   /* handler address bits 0-15  */
    uint16_t selector;    /* kernel code segment: 0x08  */
    uint8_t  ist;         /* interrupt stack table: 0   */
    uint8_t  type_attr;   /* 0x8E = present, DPL=0, 64-bit interrupt gate */
    uint16_t offset_mid;  /* handler address bits 16-31 */
    uint32_t offset_hi;   /* handler address bits 32-63 */
    uint32_t zero;        /* reserved                   */
} __attribute__((packed)) aegis_idt_gate_t;

/* Register state pushed by isr_common_stub (matches push order in isr.asm).
 * isr_dispatch receives a pointer to this struct. */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    /* CPU-pushed interrupt frame: */
    uint64_t rip, cs, rflags, rsp, ss;
} cpu_state_t;

/* Install 48 IDT gates and load with lidt.
 * Must be called before enabling any interrupts. */
void idt_init(void);

/* C-level interrupt dispatcher — called by isr_common_stub in isr.asm.
 * Marked with __attribute__((used)) to prevent dead-code elimination. */
void isr_dispatch(cpu_state_t *s);

#endif /* AEGIS_IDT_H */
```

- [ ] **Step 2: Create `idt.c`**

```c
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
```

- [ ] **Step 3: Note — idt.c will NOT compile yet (expected)**

`idt.c` includes `pit.h` and `kbd.h` which do not exist until Tasks 6 and 7. The
compiler will emit a fatal `file not found` error, not merely linker warnings.
This is expected at this stage. Do not attempt `make` until Tasks 6 and 7 are complete.
The individual file `idt.c` can be written and reviewed now; compilation is verified in Task 13.

Do not run `make` here. Proceed directly to Step 4 (commit). Compilation is verified in Task 13.

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/idt.h kernel/arch/x86_64/idt.c
git commit -m "arch: add IDT setup and isr_dispatch"
```

---

## Task 5: ISR stubs — isr.asm

**Files:**
- Create: `kernel/arch/x86_64/isr.asm`

- [ ] **Step 1: Create `isr.asm`**

```nasm
; isr.asm — Interrupt Service Routine entry stubs for Aegis
;
; Each stub pushes a uniform stack frame (fake error code for exceptions
; that don't push one), then jumps to isr_common_stub.
;
; isr_common_stub saves all GPRs, calls isr_dispatch(cpu_state_t*), restores.
;
; Vector → macro mapping (Intel SDM Vol 3A Table 6-1):
; ISR_NOERR: 0,1,2,3,4,5,6,7,9,15,16,18,19,20,28,31
; ISR_ERR:   8,10,11,12,13,14,17,21,29,30
; Reserved (install ISR_NOERR as placeholder): 22,23,24,25,26,27
; IRQ stubs (no error code): 0x20-0x2F

bits 64
section .text

extern isr_dispatch

%macro ISR_NOERR 1
global isr_%1
isr_%1:
    push qword 0    ; fake error code (uniform frame)
    push qword %1   ; vector number
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
global isr_%1
isr_%1:
    push qword %1   ; vector number (error code already on stack from CPU)
    jmp isr_common_stub
%endmacro

; CPU exceptions 0-31
ISR_NOERR  0   ; #DE divide error
ISR_NOERR  1   ; #DB debug
ISR_NOERR  2   ; NMI
ISR_NOERR  3   ; #BP breakpoint
ISR_NOERR  4   ; #OF overflow
ISR_NOERR  5   ; #BR bound range
ISR_NOERR  6   ; #UD invalid opcode
ISR_NOERR  7   ; #NM device not available
ISR_ERR    8   ; #DF double fault
ISR_NOERR  9   ; coprocessor segment overrun (reserved)
ISR_ERR   10   ; #TS invalid TSS
ISR_ERR   11   ; #NP segment not present
ISR_ERR   12   ; #SS stack fault
ISR_ERR   13   ; #GP general protection
ISR_ERR   14   ; #PF page fault
ISR_NOERR 15   ; reserved
ISR_NOERR 16   ; #MF x87 FP exception
ISR_ERR   17   ; #AC alignment check
ISR_NOERR 18   ; #MC machine check
ISR_NOERR 19   ; #XM SIMD FP exception
ISR_NOERR 20   ; #VE virtualization exception
ISR_ERR   21   ; #CP control protection (#CP has error code)
ISR_NOERR 22   ; reserved
ISR_NOERR 23   ; reserved
ISR_NOERR 24   ; reserved
ISR_NOERR 25   ; reserved
ISR_NOERR 26   ; reserved
ISR_NOERR 27   ; reserved
ISR_NOERR 28   ; #HV hypervisor injection
ISR_ERR   29   ; #VC VMM communication
ISR_ERR   30   ; #SX security exception
ISR_NOERR 31   ; reserved

; Hardware IRQs 0x20-0x2F (remapped by PIC)
ISR_NOERR 0x20 ; IRQ0 — PIT timer
ISR_NOERR 0x21 ; IRQ1 — PS/2 keyboard
ISR_NOERR 0x22 ; IRQ2 — cascade (internal)
ISR_NOERR 0x23 ; IRQ3 — COM2
ISR_NOERR 0x24 ; IRQ4 — COM1
ISR_NOERR 0x25 ; IRQ5
ISR_NOERR 0x26 ; IRQ6 — floppy
ISR_NOERR 0x27 ; IRQ7 — LPT1 / spurious master
ISR_NOERR 0x28 ; IRQ8 — RTC
ISR_NOERR 0x29 ; IRQ9
ISR_NOERR 0x2A ; IRQ10
ISR_NOERR 0x2B ; IRQ11
ISR_NOERR 0x2C ; IRQ12 — PS/2 mouse
ISR_NOERR 0x2D ; IRQ13 — FPU
ISR_NOERR 0x2E ; IRQ14 — primary ATA
ISR_NOERR 0x2F ; IRQ15 — secondary ATA / spurious slave

; Common stub — saves all GPRs, calls isr_dispatch, restores, iretq
isr_common_stub:
    ; At entry the stack holds (low to high):
    ;   vector, error_code, rip, cs, rflags, rsp, ss  (CPU-pushed)
    ; Push all GPRs in the order that matches cpu_state_t:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; RSP now points to cpu_state_t.r15 (first field pushed = r15, but
    ; cpu_state_t lays out r15 first — confirm field order matches).
    ; Pass pointer to the struct as first argument.
    mov rdi, rsp
    call isr_dispatch

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16     ; discard vector + error_code
    iretq

; Jump table — isr_stubs[i] = pointer to isr_i
; idt.c references this as: extern void *isr_stubs[48];
section .data
global isr_stubs
isr_stubs:
    dq isr_0,  isr_1,  isr_2,  isr_3,  isr_4,  isr_5,  isr_6,  isr_7
    dq isr_8,  isr_9,  isr_10, isr_11, isr_12, isr_13, isr_14, isr_15
    dq isr_16, isr_17, isr_18, isr_19, isr_20, isr_21, isr_22, isr_23
    dq isr_24, isr_25, isr_26, isr_27, isr_28, isr_29, isr_30, isr_31
    dq isr_0x20, isr_0x21, isr_0x22, isr_0x23
    dq isr_0x24, isr_0x25, isr_0x26, isr_0x27
    dq isr_0x28, isr_0x29, isr_0x2A, isr_0x2B
    dq isr_0x2C, isr_0x2D, isr_0x2E, isr_0x2F
```

**IMPORTANT — cpu_state_t field order vs push order:**

The push order in `isr_common_stub` is: rax, rbx, rcx, rdx, rsi, rdi, rbp, r8–r15.
After the push sequence, the stack from low (RSP) to high is:
`r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax, vector, error_code, rip, cs, rflags, rsp, ss`

The `cpu_state_t` in `idt.h` must match this layout exactly (first field = r15, last before CPU frame = rax). Verify they match before building.

- [ ] **Step 2: Build and verify isr.asm compiles**

```bash
make 2>&1 | head -30
```

Expected: `isr.asm` assembles. The build may now link (if all other new files have stubs). If the link fails with undefined symbols, that's fine — we still have pit/kbd/sched to write.

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/isr.asm
git commit -m "arch: add 48-vector ISR stubs and isr_common_stub"
```

---

## Task 6: PIT — timer at 100 Hz

**Files:**
- Create: `kernel/arch/x86_64/pit.h`
- Create: `kernel/arch/x86_64/pit.c`

- [ ] **Step 1: Create `pit.h`**

```c
#ifndef AEGIS_PIT_H
#define AEGIS_PIT_H

#include <stdint.h>

/* Program PIT channel 0 at 100 Hz. Unmasks IRQ0.
 * Prints [PIT] OK: timer at 100 Hz. */
void pit_init(void);

/* Called by isr_dispatch on vector 0x20 (after EOI is sent).
 * Increments tick counter, then calls sched_tick(). */
void pit_handler(void);

#endif /* AEGIS_PIT_H */
```

- [ ] **Step 2: Create `pit.c`**

```c
#include "pit.h"
#include "pic.h"
#include "arch.h"
#include "printk.h"

#define PIT_CHANNEL0 0x40
#define PIT_CMD      0x43
/* Channel 0, lobyte/hibyte, mode 3 (square wave) */
#define PIT_MODE     0x36
/* 1193182 Hz / 100 = 11931.82 → round to 11932 */
#define PIT_DIVISOR  11932

/* File-static: never accessed outside pit.c.
 * kernel/core/ uses arch_get_ticks() declared in arch.h. */
static volatile uint64_t s_ticks = 0;

/* Forward declaration: sched_tick is implemented in kernel/sched/sched.c.
 * We use a forward decl here to avoid a circular include dependency.
 * -Ikernel/sched is in CFLAGS so we could include sched.h, but the
 * forward decl is cleaner for a single-function dependency. */
void sched_tick(void);

void
pit_init(void)
{
    /* Program PIT channel 0 */
    outb(PIT_CMD, PIT_MODE);
    outb(PIT_CHANNEL0, PIT_DIVISOR & 0xFF);        /* low byte  */
    outb(PIT_CHANNEL0, (PIT_DIVISOR >> 8) & 0xFF); /* high byte */

    /* Unmask IRQ0 so the PIT starts firing */
    pic_unmask(0);

    printk("[PIT] OK: timer at 100 Hz\n");
}

void
pit_handler(void)
{
    s_ticks++;
    sched_tick();
}

/* arch_get_ticks — arch-boundary accessor for the tick counter.
 * Declared in arch.h so kernel/core/ can call it without including pit.h. */
uint64_t
arch_get_ticks(void)
{
    return s_ticks;
}
```

- [ ] **Step 3: Verify pit.c compiles**

```bash
make 2>&1 | grep -E "pit|error:" | head -10
```

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/pit.h kernel/arch/x86_64/pit.c
git commit -m "arch: add PIT driver at 100 Hz with arch_get_ticks()"
```

---

## Task 7: PS/2 Keyboard

**Files:**
- Create: `kernel/arch/x86_64/kbd.h`
- Create: `kernel/arch/x86_64/kbd.c`

- [ ] **Step 1: Create `kbd.h`**

```c
#ifndef AEGIS_KBD_H
#define AEGIS_KBD_H

/* Initialize PS/2 keyboard. Unmasks IRQ1.
 * Prints [KBD] OK: PS/2 keyboard ready. */
void kbd_init(void);

/* Called by isr_dispatch on vector 0x21.
 * Reads scancode from port 0x60, converts to ASCII, pushes to ring buffer.
 * Break codes (bit 7 set) and 0xE0 extended scancodes are silently dropped. */
void kbd_handler(void);

/* Blocking read — spins until a character is available in the ring buffer. */
char kbd_read(void);

/* Non-blocking read. Returns 1 and writes to *out if a char is available.
 * Returns 0 if the buffer is empty. */
int kbd_poll(char *out);

#endif /* AEGIS_KBD_H */
```

- [ ] **Step 2: Create `kbd.c`**

```c
#include "kbd.h"
#include "pic.h"
#include "arch.h"
#include "printk.h"

#define KBD_DATA 0x60

/* 64-byte ring buffer */
#define KBD_BUF_SIZE 64
static volatile char    s_buf[KBD_BUF_SIZE];
static volatile uint8_t s_head = 0;  /* next write position */
static volatile uint8_t s_tail = 0;  /* next read position  */

/* Shift state */
static volatile int s_shift = 0;

/* US QWERTY scancode set 1 — unshifted (make codes 0x01–0x39) */
static const char s_sc_lower[] = {
    0,    0,   '1', '2', '3', '4', '5', '6',  /* 0x00–0x07 */
    '7', '8', '9', '0', '-', '=',  '\b', '\t', /* 0x08–0x0F */
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',   /* 0x10–0x17 */
    'o', 'p', '[', ']', '\n',  0,  'a', 's',   /* 0x18–0x1F */
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',   /* 0x20–0x27 */
    '\'', '`', 0,  '\\','z', 'x', 'c', 'v',   /* 0x28–0x2F */
    'b', 'n', 'm', ',', '.', '/',  0,   '*',   /* 0x30–0x37 */
    0,   ' '                                    /* 0x38–0x39 */
};

/* US QWERTY scancode set 1 — shifted */
static const char s_sc_upper[] = {
    0,    0,   '!', '@', '#', '$', '%', '^',
    '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    'O', 'P', '{', '}', '\n',  0,  'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"', '~',  0,  '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?',  0,   '*',
    0,   ' '
};

#define SC_TABLE_SIZE ((int)(sizeof(s_sc_lower) / sizeof(s_sc_lower[0])))

static void
buf_push(char c)
{
    uint8_t next = (s_head + 1) & (KBD_BUF_SIZE - 1);
    if (next != s_tail) {   /* drop if full */
        s_buf[s_head] = c;
        s_head = next;
    }
}

void
kbd_init(void)
{
    pic_unmask(1);  /* IRQ1 = PS/2 keyboard */
    printk("[KBD] OK: PS/2 keyboard ready\n");
}

void
kbd_handler(void)
{
    uint8_t sc = inb(KBD_DATA);

    /* Extended key prefix — skip this byte (next byte is the actual key) */
    if (sc == 0xE0)
        return;

    /* Break code (key released) — bit 7 set */
    if (sc & 0x80) {
        uint8_t make = sc & 0x7F;
        /* Track shift releases */
        if (make == 0x2A || make == 0x36)
            s_shift = 0;
        return;
    }

    /* Make code */
    if (sc == 0x2A || sc == 0x36) {    /* left or right shift */
        s_shift = 1;
        return;
    }

    if (sc < SC_TABLE_SIZE) {
        char c = s_shift ? s_sc_upper[sc] : s_sc_lower[sc];
        if (c)
            buf_push(c);
    }
}

char
kbd_read(void)
{
    char c;
    while (!kbd_poll(&c))
        ;   /* spin — task_kbd yields on next timer tick */
    return c;
}

int
kbd_poll(char *out)
{
    if (s_head == s_tail)
        return 0;
    *out = s_buf[s_tail];
    s_tail = (s_tail + 1) & (KBD_BUF_SIZE - 1);
    return 1;
}
```

- [ ] **Step 3: Verify kbd.c compiles**

```bash
make 2>&1 | grep -E "kbd|error:" | head -10
```

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/kbd.h kernel/arch/x86_64/kbd.c
git commit -m "arch: add PS/2 keyboard driver with US QWERTY scancode table"
```

---

## Task 8: VGA enhancements — cursor, scroll, vga_putchar

**Files:**
- Modify: `kernel/arch/x86_64/vga.h`
- Modify: `kernel/arch/x86_64/vga.c`

- [ ] **Step 1: Read vga.h and vga.c**

```bash
cat kernel/arch/x86_64/vga.h
cat kernel/arch/x86_64/vga.c
```

- [ ] **Step 2: Add `vga_putchar` declaration to vga.h**

Add after `vga_write_char`:
```c
/* Write a single character with full cursor tracking, scrolling, and
 * interrupt-flag preservation (pushfq/popfq). Use this for interactive
 * output. vga_write_string routes through vga_putchar. */
void vga_putchar(char c);
```

- [ ] **Step 3: Rewrite the output section of vga.c**

Add static cursor state near the top of vga.c (after the existing static VGA buffer pointer):
```c
#define VGA_COLS 80
#define VGA_ROWS 25
static int s_row = 0;
static int s_col = 0;
```

Replace `vga_write_char` (and update `vga_write_string`) with:

```c
static void
vga_set_cursor(int row, int col)
{
    uint16_t pos = (uint16_t)(row * VGA_COLS + col);
    outb(0x3D4, 0x0F); outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E); outb(0x3D5, (pos >> 8) & 0xFF);
}

static void
vga_scroll(void)
{
    /* Copy rows 1-24 up to rows 0-23 */
    volatile uint16_t *vga = (volatile uint16_t *)0xFFFFFFFF800B8000UL;
    for (int r = 0; r < VGA_ROWS - 1; r++)
        for (int c = 0; c < VGA_COLS; c++)
            vga[r * VGA_COLS + c] = vga[(r + 1) * VGA_COLS + c];
    /* Clear bottom row */
    for (int c = 0; c < VGA_COLS; c++)
        vga[(VGA_ROWS - 1) * VGA_COLS + c] = (uint16_t)(' ' | (0x07 << 8));
    s_row = VGA_ROWS - 1;
}

void
vga_putchar(char c)
{
    /* Save and restore RFLAGS.IF — safe before IDT is installed (IF=0,
     * popfq restores 0, never fires sti), safe in ISR context (IF=0),
     * and correct in task context (IF=1, restored after write). */
    uint64_t flags;
    __asm__ volatile ("pushfq; cli; popq %0" : "=r"(flags) : : "memory");

    volatile uint16_t *vga = (volatile uint16_t *)0xFFFFFFFF800B8000UL;

    if (c == '\n') {
        s_col = 0;
        s_row++;
    } else if (c == '\r') {
        s_col = 0;
    } else if (c == '\b') {
        if (s_col > 0) {
            s_col--;
            vga[s_row * VGA_COLS + s_col] = (uint16_t)(' ' | (0x07 << 8));
        }
    } else {
        vga[s_row * VGA_COLS + s_col] = (uint16_t)((uint8_t)c | (0x07 << 8));
        s_col++;
        if (s_col >= VGA_COLS) {
            s_col = 0;
            s_row++;
        }
    }

    if (s_row >= VGA_ROWS)
        vga_scroll();

    if (vga_available)
        vga_set_cursor(s_row, s_col);

    __asm__ volatile ("pushq %0; popfq" : : "r"(flags) : "memory");
}

void
vga_write_char(char c)
{
    vga_putchar(c);
}

void
vga_write_string(const char *s)
{
    while (*s)
        vga_putchar(*s++);
}
```

**IMPORTANT:** Check the existing VGA framebuffer address in vga.c. After Phase 3, the kernel runs at `0xFFFFFFFF80000000`. The VGA framebuffer at physical `0xB8000` is identity-mapped, so its virtual address is the same `0xB8000` under identity mapping OR `0xFFFFFFFF800B8000` under the higher-half mapping. Use whichever the existing vga.c uses — do not change it if it already works.

- [ ] **Step 4: Verify vga.c compiles and make test still works (even though it's RED)**

```bash
make 2>&1 | grep -E "vga|error:" | head -10
```

- [ ] **Step 5: Commit**

```bash
git add kernel/arch/x86_64/vga.h kernel/arch/x86_64/vga.c
git commit -m "arch(vga): add vga_putchar with cursor, scroll, pushfq/popfq guard"
```

---

## Task 9: Context switch — ctx_switch.asm

**Files:**
- Create: `kernel/arch/x86_64/ctx_switch.asm`

- [ ] **Step 1: Create `ctx_switch.asm`**

```nasm
; ctx_switch.asm — preemptive context switch for Aegis scheduler
;
; Saves callee-saved registers (rbx, rbp, r12-r15) for the outgoing task
; and restores them for the incoming task. Swaps RSP via the TCB's rsp field.
;
; The compiler already saves caller-saved registers (rax, rcx, rdx, rsi,
; rdi, r8-r11) at any call site, so we only need callee-saved here.
;
; Calling convention: System V AMD64 ABI
;   rdi = pointer to current task's aegis_task_t  (outgoing)
;   rsi = pointer to next task's aegis_task_t     (incoming)
;
; Clobbers: RSP (switches to new task's stack). All callee-saved registers
;   are preserved across the call from each task's perspective.
;
; New task entry: sched_spawn sets up the stack so the first ctx_switch
;   into a new task "returns" into the task's entry function. Stack layout
;   (low to high, RSP points at r15 slot):
;     [r15=0][r14=0][r13=0][r12=0][rbp=0][rbx=0][fn]
;   After pops: r15-rbx restored to 0, ret pops fn into RIP.

bits 64
section .text

global ctx_switch
ctx_switch:
    ; Save outgoing task's callee-saved registers
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; current->rsp = rsp  (rsp field is at offset 0 — verified by _Static_assert)
    mov [rdi], rsp

    ; rsp = next->rsp
    mov rsp, [rsi]

    ; Restore incoming task's callee-saved registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; Return into the incoming task (pops its saved RIP from the stack)
    ret
```

- [ ] **Step 2: Assemble and verify**

```bash
make 2>&1 | grep -E "ctx_switch|error:" | head -10
```

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/ctx_switch.asm
git commit -m "arch: add ctx_switch assembly for preemptive context switching"
```

---

## Task 10: Scheduler — sched.h + sched.c

**Files:**
- Create: `kernel/sched/sched.h`
- Create: `kernel/sched/sched.c`

(The `kernel/sched/` directory does not exist yet — create it.)

- [ ] **Step 1: Create `kernel/sched/sched.h`**

```c
#ifndef AEGIS_SCHED_H
#define AEGIS_SCHED_H

#include <stdint.h>

typedef struct aegis_task_t {
    uint64_t             rsp;         /* MUST be first — ctx_switch reads [rdi+0] */
    uint8_t             *stack_base;  /* bottom of PMM-allocated stack (for future cleanup) */
    uint32_t             tid;         /* task ID */
    struct aegis_task_t *next;        /* circular linked list */
} aegis_task_t;

/* Initialize the run queue. No tasks yet. */
void sched_init(void);

/* Allocate a TCB and 16KB stack from PMM; wire fn as entry point; add to queue. */
void sched_spawn(void (*fn)(void));

/* Print [SCHED] OK line, then switch directly into the first task via a
 * dummy TCB (one-way ctx_switch). Does not return. Each task enables
 * interrupts on entry via `sti`. */
void sched_start(void);

/* Called by pit_handler on each timer tick.
 * Advances current task and calls ctx_switch. */
void sched_tick(void);

#endif /* AEGIS_SCHED_H */
```

- [ ] **Step 2: Create `kernel/sched/sched.c`**

```c
#include "sched.h"
#include "arch.h"
#include "pmm.h"
#include "printk.h"
#include <stddef.h>

/* Compile-time guard: ctx_switch.asm assumes rsp is at offset 0 of TCB.
 * If anyone adds a field before rsp, this catches it immediately. */
_Static_assert(offsetof(aegis_task_t, rsp) == 0,
    "rsp must be first field in aegis_task_t — ctx_switch depends on this");

#define STACK_PAGES  4                     /* 16KB per task */
#define STACK_SIZE   (STACK_PAGES * 4096UL)

static aegis_task_t *s_current = (void *)0;
static uint32_t      s_next_tid = 0;
static uint32_t      s_task_count = 0;

void
sched_init(void)
{
    s_current   = (void *)0;
    s_next_tid  = 0;
    s_task_count = 0;
}

void
sched_spawn(void (*fn)(void))
{
    /* Allocate TCB (one page from PMM — plenty of space) */
    uint64_t tcb_phys = pmm_alloc_page();
    if (!tcb_phys) {
        printk("[SCHED] FAIL: OOM allocating TCB\n");
        for (;;) {}
    }
    aegis_task_t *task = (aegis_task_t *)(uintptr_t)tcb_phys;

    /* Allocate stack (STACK_PAGES individual pages).
     *
     * CONTIGUITY ASSUMPTION: The Phase 3 PMM is a bitmap allocator over the
     * physical memory map. Early boot memory is a single contiguous range and
     * the bitmap allocates sequentially, so successive pmm_alloc_page() calls
     * return physically adjacent frames. This allows treating the pages as a
     * single STACK_SIZE region. If the PMM ever becomes non-sequential (e.g.
     * after buddy allocator introduction in Phase 5), this must be replaced
     * with a multi-page contiguous allocation.
     */
    uint8_t *stack = (void *)0;
    for (uint32_t i = 0; i < STACK_PAGES; i++) {
        uint64_t p = pmm_alloc_page();
        if (!p) {
            printk("[SCHED] FAIL: OOM allocating stack\n");
            for (;;) {}
        }
        if (i == 0)
            stack = (uint8_t *)(uintptr_t)p;
    }

    /* Set up the stack to look like ctx_switch already ran.
     *
     * ctx_switch pops: r15, r14, r13, r12, rbp, rbx, then ret → fn.
     * So the stack from low (RSP) to high must be:
     *   [r15=0][r14=0][r13=0][r12=0][rbp=0][rbx=0][fn]
     *
     * We build this by decrementing a pointer from stack_top:
     *   fn pushed first (deepest = highest address before RSP setup)
     *   then six zeros for the callee-saved regs
     *   RSP ends up pointing at the r15 slot.
     */
    uint64_t *sp = (uint64_t *)(stack + STACK_SIZE);
    *--sp = (uint64_t)fn;   /* return address: ret jumps here */
    *--sp = 0;              /* rbx */
    *--sp = 0;              /* rbp */
    *--sp = 0;              /* r12 */
    *--sp = 0;              /* r13 */
    *--sp = 0;              /* r14 */
    *--sp = 0;              /* r15  ← new task's RSP */

    task->rsp        = (uint64_t)sp;
    task->stack_base = stack;
    task->tid        = s_next_tid++;

    /* Add to circular list */
    if (!s_current) {
        task->next  = task;
        s_current   = task;
    } else {
        /* Insert after current */
        task->next      = s_current->next;
        s_current->next = task;
    }

    s_task_count++;
}

void
sched_start(void)
{
    printk("[SCHED] OK: scheduler started, %u tasks\n", s_task_count);

    /* One-way switch into the first task.
     *
     * IMPORTANT: Do NOT call sti and return here. If we returned to the idle
     * hlt loop and the first PIT tick fired from there, sched_tick would call
     * ctx_switch(task_kbd, task_heartbeat) while RSP is deep in the ISR frame.
     * ctx_switch would save the ISR stack pointer into task_kbd->rsp, corrupting
     * the TCB. Resuming task_kbd later would load a garbage RSP and crash.
     *
     * Fix: switch directly into the first task using a stack-local dummy TCB.
     * ctx_switch saves our current RSP into dummy.rsp (which we immediately
     * abandon). The first task starts on its own correctly-constructed initial
     * stack. Each task calls sti at startup to enable interrupts from task context.
     *
     * sched_start() never returns.
     */
    aegis_task_t dummy;
    ctx_switch(&dummy, s_current);
    __builtin_unreachable();
}

void
sched_tick(void)
{
    if (!s_current || !s_current->next)
        return;

    aegis_task_t *old = s_current;
    s_current = s_current->next;

    /* ctx_switch is declared in arch.h with a forward struct declaration.
     * It saves old->rsp, loads s_current->rsp, and returns into new task. */
    ctx_switch(old, s_current);
}
```

- [ ] **Step 3: Verify sched.c compiles**

```bash
make 2>&1 | grep -E "sched|error:" | head -10
```

- [ ] **Step 4: Commit**

```bash
git add kernel/sched/sched.h kernel/sched/sched.c
git commit -m "sched: add preemptive round-robin scheduler with _Static_assert guard"
```

---

## Task 11: arch.h — final new declarations

**Files:**
- Modify: `kernel/arch/x86_64/arch.h`

- [ ] **Step 1: Read arch.h to see current state**

```bash
cat kernel/arch/x86_64/arch.h
```

- [ ] **Step 2: Add Phase 4 declarations**

Add the following section to `arch.h` after the VMM interface section:

```c
/* -------------------------------------------------------------------------
 * Phase 4: Interrupt infrastructure
 * ------------------------------------------------------------------------- */

/* IDT: install 48 interrupt gate descriptors and load with lidt. */
void idt_init(void);

/* PIC: remap 8259A dual PIC so IRQ0-15 land at vectors 0x20-0x2F.
 * Masks all IRQs after init; drivers call pic_unmask(irq) when ready. */
void pic_init(void);

/* PIT: program channel 0 at 100 Hz and unmask IRQ0. */
void pit_init(void);

/* Returns the current PIT tick count (incremented 100x/second).
 * Use this instead of accessing the pit.c-internal counter directly. */
uint64_t arch_get_ticks(void);

/* -------------------------------------------------------------------------
 * Phase 4: Keyboard
 * ------------------------------------------------------------------------- */

/* Initialize PS/2 keyboard and unmask IRQ1. */
void kbd_init(void);

/* Blocking read — spins until a keypress is available. */
char kbd_read(void);

/* Non-blocking read. Returns 1 if a char was available and written to *out. */
int kbd_poll(char *out);

/* -------------------------------------------------------------------------
 * Phase 4: Scheduler (arch boundary — ctx_switch is implemented in asm)
 * ------------------------------------------------------------------------- */

/* Forward declaration for the TCB type (defined in kernel/sched/sched.h).
 * arch.h declares ctx_switch here so pit.c and others can call it without
 * including sched.h, keeping the arch-agnostic sched layer separate. */
struct aegis_task_t;

/* Switch execution from 'current' to 'next'.
 * Saves callee-saved registers + RSP for current; restores for next.
 * Clobbers: RSP (switches to new task stack). Implemented in ctx_switch.asm. */
void ctx_switch(struct aegis_task_t *current, struct aegis_task_t *next);
```

- [ ] **Step 3: Build and verify no new errors**

```bash
make 2>&1 | head -30
```

At this point the build may succeed or fail only at link — all C files should compile cleanly.

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/arch.h
git commit -m "arch: add Phase 4 declarations to arch.h (IDT, PIC, PIT, KBD, ctx_switch)"
```

---

## Task 12: main.c — wire Phase 4 into the boot sequence

**Files:**
- Modify: `kernel/core/main.c`

- [ ] **Step 1: Read the current main.c**

```bash
cat kernel/core/main.c
```

- [ ] **Step 2: Rewrite main.c**

Replace the entire file with:

```c
#include "arch.h"
#include "printk.h"
#include "cap.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"

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
    uint64_t last = 0;
    for (;;) {
        uint64_t t = arch_get_ticks();
        if (t >= 500) {
            printk("[AEGIS] System halted.\n");
            /* FIXME: arch_debug_exit called from within a scheduled task.
             * Stack state is indeterminate. Acceptable for Phase 4 because
             * isa-debug-exit writes to an I/O port and QEMU exits immediately.
             * Phase 5+ must implement a clean kernel shutdown path. */
            arch_debug_exit(0x01);
        }
        if (t - last >= 100) {
            last = t;
        }
    }
}

void
kernel_main(uint32_t mb_magic, void *mb_info)
{
    (void)mb_magic;

    arch_init();            /* serial_init + vga_init                        */
    arch_mm_init(mb_info);  /* parse multiboot2 memory map                   */
    pmm_init();             /* bitmap allocator — [PMM] OK                   */
    vmm_init();             /* page tables, higher-half map — [VMM] OK       */
    cap_init();             /* capability stub — [CAP] OK                    */
    idt_init();             /* 48 interrupt gates — [IDT] OK                 */
    pic_init();             /* remap 8259A — [PIC] OK                        */
    pit_init();             /* 100 Hz timer — [PIT] OK                       */
    kbd_init();             /* PS/2 keyboard — [KBD] OK                      */
    sched_init();           /* init run queue (no tasks yet)                 */
    sched_spawn(task_kbd);
    sched_spawn(task_heartbeat);
    sched_start();          /* prints [SCHED] OK, switches into first task   */
    /* UNREACHABLE — sched_start() never returns */
    __builtin_unreachable();
}
```

- [ ] **Step 3: Build — must link cleanly now**

```bash
make 2>&1
```

Expected: clean build, no errors. If there are undefined symbol errors, read them carefully — a missing `extern` or wrong function name in one of the new files.

- [ ] **Step 4: Commit**

```bash
git add kernel/core/main.c
git commit -m "core: wire Phase 4 into boot sequence (IDT/PIC/PIT/KBD/SCHED)"
```

---

## Task 13: GREEN — make test passes

**Files:** none (verification only)

- [ ] **Step 1: Run make test**

```bash
make test
```

Expected: exits 0. The heartbeat task fires `arch_debug_exit` after 500 ticks (~5 seconds), within the 10-second QEMU timeout.

**If make test fails**, the most likely causes:

| Symptom | Likely cause |
|---------|-------------|
| Kernel triple-faults on boot | VGA buffer address wrong after Phase 3 higher-half mapping; or `outb` in `vga_putchar` fires before IDT when IF gets set |
| `[IDT]` missing, kernel hangs | `idt_init` called before `pic_init` — spurious interrupts before PIC is remapped may fire an unhandled vector |
| `[SCHED]` wrong task count | `sched_start()` counts tasks in a queue that uses circular insertion — verify the count increments on each `sched_spawn` |
| `[AEGIS]` line never appears | Heartbeat task never runs — scheduler may have a bug in initial task setup or context switch stack frame |
| Diff shows extra/wrong lines | A printk in a task fires before the scheduler is started — check for bare printk calls that bypass serial ordering |

- [ ] **Step 2: Run make run to verify interactive behavior**

```bash
make run
```

QEMU opens. You should see the boot lines on serial and a blinking cursor on VGA. Type a few keys — they should echo. The kernel runs indefinitely under `make run` (no 500-tick exit).

- [ ] **Step 3: Commit if any fixes were needed**

If Task 13 required fixes, commit them before proceeding.

---

## Task 14: Update CLAUDE.md

**Files:**
- Modify: `.claude/CLAUDE.md`

- [ ] **Step 1: Read the Build Status table in CLAUDE.md**

- [ ] **Step 2: Update the Scheduler row and add new rows**

Find and update these rows in the Build Status table:

```markdown
| Virtual memory / paging    | ✅ Done        | Higher-half kernel at 0xFFFFFFFF80000000; 5-table setup (identity + kernel); identity map kept; teardown deferred to Phase 4 |
| Scheduler (single-core)    | ✅ Done        | Preemptive round-robin; PIT at 100 Hz; callee-saved context switch; 2 kernel tasks; PS/2 keyboard echo |
| Syscall dispatch (Rust)    | ⬜ Not started | |
| Capability system (Rust)   | ✅ Done        | Stub only: cap_init() prints OK line |
```

Also add entries for the new subsystems:
```markdown
| IDT (48 vectors)           | ✅ Done        | Interrupt gates, isr_common_stub, cpu_state_t; EOI sent before handler |
| PIC (8259A)                | ✅ Done        | IRQ0-15 remapped to 0x20-0x2F; all masked except IRQ0/IRQ1 |
| PIT (100 Hz)               | ✅ Done        | Channel 0 at 100 Hz; arch_get_ticks() arch-boundary accessor |
| PS/2 keyboard              | ✅ Done        | US QWERTY scancode set 1; 64-byte ring buffer; shift tracking |
| VGA (enhanced)             | ✅ Done        | vga_putchar with cursor, scroll, backspace; pushfq/popfq interrupt guard |
```

- [ ] **Step 3: Update the last-updated line**

```markdown
*Last updated: 2026-03-20 — Phase 4 complete; preemptive scheduler live; interactive VGA terminal.*
```

- [ ] **Step 4: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs: mark Phase 4 done in CLAUDE.md build status"
```
