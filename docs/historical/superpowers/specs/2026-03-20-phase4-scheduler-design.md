# Phase 4: IDT + PIC + PIT + PS/2 Keyboard + Preemptive Scheduler

## Design Spec — 2026-03-20

---

## 1. Goal

Add a preemptive round-robin scheduler driven by a hardware timer (PIT at 100 Hz),
a PS/2 keyboard driver with ring buffer, and an enhanced VGA terminal with cursor
and scrolling. Result: two concurrent kernel tasks running under interrupt-driven
preemption — one echoes keyboard input, one prints a heartbeat counter.

`make test` exits 0. `make run` shows an interactive VGA terminal.

---

## 2. Architecture Overview

```
IDT (48 vectors)
 └─ PIC (8259A, IRQs remapped to 0x20–0x2F)
     ├─ IRQ0 → PIT (100 Hz) → sched_tick() → ctx_switch()
     └─ IRQ1 → PS/2 kbd → ring buffer → kbd_read()

SCHED: circular TCB list, 16KB stacks from PMM
VGA:   cursor tracking, hardware cursor, scrolling, backspace
```

Subsystem init order in `kernel_main`:
```
arch_init → arch_mm_init → pmm_init → vmm_init → cap_init
→ idt_init → pic_init → pit_init → kbd_init → sched_init → sched_start
```

`sched_start()` enables interrupts (`sti`) and returns. The PIT takes over.

---

## 3. New Files

| File | Section | Purpose |
|------|---------|---------|
| `kernel/arch/x86_64/idt.h` + `idt.c` | 3.1 | IDT table, gate descriptors, `lidt` |
| `kernel/arch/x86_64/isr.asm` | 3.2 | 48 ISR entry stubs + common stub |
| `kernel/arch/x86_64/pic.h` + `pic.c` | 3.3 | 8259A remap, mask, EOI |
| `kernel/arch/x86_64/pit.h` + `pit.c` | 3.4 | PIT channel 0 at 100 Hz |
| `kernel/arch/x86_64/kbd.h` + `kbd.c` | 3.5 | PS/2 scancode→ASCII, ring buffer |
| `kernel/arch/x86_64/ctx_switch.asm` | 3.6 | Callee-saved reg swap + RSP swap |
| `kernel/sched/sched.h` + `sched.c` | 3.7 | TCB, circular run queue, spawn, tick |

## 4. Modified Files

| File | Change |
|------|--------|
| `kernel/arch/x86_64/vga.c` + `vga.h` | Cursor state, scroll, `vga_putchar`, hardware cursor |
| `kernel/arch/x86_64/arch.h` | Add: `idt_init`, `pic_init`, `pit_init`, `kbd_init`, `kbd_read`, `kbd_poll`, `arch_get_ticks`; update `ctx_switch` declaration; note `vga_write_string` now routes through `vga_putchar` |
| `kernel/core/main.c` | Wire new inits, spawn 2 tasks, call sched_start |
| `Makefile` | Add new source files to ARCH_SRCS and SCHED_SRCS |
| `tests/expected/boot.txt` | 5 new subsystem lines |
| `.claude/CLAUDE.md` | Build status updates |

---

## 5. Detailed Design

### 5.1 IDT (`idt.c` / `idt.h`)

```c
typedef struct {
    uint16_t offset_lo;
    uint16_t selector;      /* 0x08 — kernel code segment */
    uint8_t  ist;           /* 0 — no IST */
    uint8_t  type_attr;     /* 0x8E — interrupt gate, DPL=0, present */
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t zero;
} __attribute__((packed)) aegis_idt_gate_t;

/* 48 entries: vectors 0x00–0x2F */
static aegis_idt_gate_t s_idt[48];
```

`idt_set_gate(uint8_t vector, void *handler)` fills one entry.
`idt_init()` calls `idt_set_gate` for all 48 vectors using the stubs from `isr.asm`,
loads with `lidt`, prints `[IDT] OK: 48 vectors installed`.

Type attribute `0x8E`: present=1, DPL=0, type=interrupt gate (clears IF on entry).

### 5.2 ISR Stubs (`isr.asm`)

NASM macro generates 48 stubs. Exceptions without an error code push a fake 0
so the frame is uniform. Each stub pushes its vector number and jumps to
`isr_common_stub`.

```nasm
%macro ISR_NOERR 1
isr_%1:
    push qword 0      ; fake error code
    push qword %1     ; vector number
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
isr_%1:
    push qword %1     ; vector number (error code already on stack)
    jmp isr_common_stub
%endmacro
```

Exceptions with CPU-pushed error codes (per Intel SDM Vol 3A Table 6-1):
8, 10, 11, 12, 13, 14, 17, 21, 29, 30.

The top of `isr.asm` **must** include an explicit comment block listing every
vector with its macro (ISR_ERR or ISR_NOERR) before the macro invocations.
One misclassified vector misaligns the interrupt frame and produces a fault
inside the fault handler — a crash that is genuinely hard to diagnose:

```nasm
; Vector → macro mapping (Intel SDM Vol 3A Table 6-1)
; ISR_NOERR: 0,1,2,3,4,5,6,7,9,15,16,18,19,20,28,31
; ISR_ERR:   8,10,11,12,13,14,17,21,29,30
; Reserved (install ISR_NOERR as placeholder): 22,23,24,25,26,27
```

`isr_common_stub` (bits 64):
1. Push rax, rbx, rcx, rdx, rsi, rdi, rbp, r8–r15 (15 regs)
2. `mov rdi, rsp` → pointer to `cpu_state_t` as first arg
3. `call isr_dispatch`
4. Pop regs in reverse
5. `add rsp, 16` (skip vector + error code)
6. `iretq`

```c
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    /* CPU-pushed: */
    uint64_t rip, cs, rflags, rsp, ss;
} cpu_state_t;
```

`isr_dispatch(cpu_state_t *s)` in `idt.c` — must use explicit `else if` chain:

```c
void isr_dispatch(cpu_state_t *s) {
    if (s->vector < 32) {
        /* CPU exception: panic */
        printk("[PANIC] exception %lu at RIP=0x%lx\n", s->vector, s->rip);
        for (;;) {}
    } else if (s->vector < 0x30) {
        /* Hardware IRQ: send EOI BEFORE calling handler.
         * EOI must arrive before any context switch, because ctx_switch
         * replaces RSP — if we switch away first, EOI is stranded on the
         * outgoing task's return path and IRQ0 is silenced until that task
         * is next scheduled. */
        uint8_t irq = (uint8_t)(s->vector - 0x20);
        pic_send_eoi(irq);
        if      (s->vector == 0x20) { pit_handler(); }
        else if (s->vector == 0x21) { kbd_handler(); }
        /* other IRQs: EOI already sent, nothing else to do */
    }
    /* spurious vectors above 0x2F: ignored */
}
```

Note: `pit_handler()` calls `sched_tick()` which calls `ctx_switch()`. Because EOI
is sent before the handler, the PIC is ready for the next tick even if `ctx_switch`
switches away and this function never returns on the outgoing task.

### 5.3 PIC (`pic.c` / `pic.h`)

8259A dual-PIC remap. Master: IRQ0–7 → vectors 0x20–0x27. Slave: IRQ8–15 → 0x28–0x2F.

```c
void pic_init(void);                  /* remap + mask all except IRQ0, IRQ1 */
void pic_send_eoi(uint8_t irq);       /* send EOI to master (and slave if irq >= 8) */
void pic_unmask(uint8_t irq);
void pic_mask(uint8_t irq);
```

`pic_init()` prints `[PIC] OK: IRQ0-15 remapped to vectors 0x20-0x2F`.

### 5.4 PIT (`pit.c` / `pit.h`)

Channel 0, mode 3 (square wave), divisor 11932 → ~100.03 Hz.

```c
#define PIT_HZ       100
#define PIT_DIVISOR  11932   /* 1193182 / 100 */

void     pit_init(void);        /* program PIT, unmask IRQ0 */
void     pit_handler(void);     /* called by isr_dispatch on vector 0x20 */
uint64_t arch_get_ticks(void);  /* returns current tick count; declared in arch.h */
```

`pit_handler()` increments a file-static `s_ticks`, then calls `sched_tick()`.
`arch_get_ticks()` is the arch-boundary accessor — `kernel/core/` and `kernel/sched/`
use this instead of referencing the x86-specific `g_ticks` directly.
`pit_init()` prints `[PIT] OK: timer at 100 Hz`.

### 5.5 PS/2 Keyboard (`kbd.c` / `kbd.h`)

**Ring buffer:** 64-byte circular buffer. IRQ1 handler reads port 0x60, converts
scancode (set 1) to ASCII, pushes to buffer. Ignores break codes (bit 7 set).
Tracks left/right shift state via scancodes 0x2A, 0x36 (make) and 0xAA, 0xB6 (break).

```c
void kbd_init(void);          /* unmask IRQ1, prints [KBD] OK */
void kbd_handler(void);       /* called by isr_dispatch on vector 0x21 */
char kbd_read(void);          /* blocking: spin until char available */
int  kbd_poll(char *out);     /* non-blocking: returns 1 if char available */
```

Scancode table covers: a–z, A–Z (with shift), 0–9, space, enter (`\n`),
backspace (`\b`), common punctuation. Extended (0xE0) and non-ASCII scancodes
are silently dropped.

`kbd_init()` prints `[KBD] OK: PS/2 keyboard ready`.

### 5.6 Context Switch (`ctx_switch.asm`)

Saves/restores only callee-saved registers (rbx, rbp, r12–r15) because the
C compiler already saves caller-saved registers at call sites.

```nasm
; ctx_switch — switch execution from 'current' to 'next'
; Arguments (SysV AMD64): rdi = current aegis_task_t*, rsi = next aegis_task_t*
; Clobbers: none (all saved/restored)
; Convention: C-callable
global ctx_switch
ctx_switch:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov  [rdi], rsp      ; current->rsp = rsp  (rsp is first field of TCB)
    mov  rsp, [rsi]      ; rsp = next->rsp
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbp
    pop  rbx
    ret                  ; returns into next task
```

**New task stack setup** in `sched_spawn(fn)`:

The stack must look as if `ctx_switch` was already called on this task. Since
`ctx_switch` pops r15 first and `ret`s last, the layout from high → low address is:

```
[fn address]   ← pushed first (deepest — this is what ret pops)
[rbx = 0]
[rbp = 0]
[r12 = 0]
[r13 = 0]
[r14 = 0]
[r15 = 0]      ← RSP points here after setup
```

In C (stack grows downward; `stack_top` is highest address of allocated region):
```c
uint64_t *sp = (uint64_t *)stack_top;
*--sp = (uint64_t)fn;   /* return address: ret jumps here */
*--sp = 0;              /* rbx */
*--sp = 0;              /* rbp */
*--sp = 0;              /* r12 */
*--sp = 0;              /* r13 */
*--sp = 0;              /* r14 */
*--sp = 0;              /* r15  ← new task's saved RSP */
task->rsp = (uint64_t)sp;
```

**Push order matters**: `fn` must be pushed first (deepest in the stack) so that
after `ctx_switch` pops r15–rbx (6 registers), `ret` finds `fn` as the return
address. Pushing `fn` last (shallowest) would cause `ret` to pop r15 instead.

### 5.7 Scheduler (`sched.c` / `sched.h`)

```c
typedef struct aegis_task_t {
    uint64_t             rsp;         /* MUST be first — ctx_switch reads [rdi+0] */
    uint8_t             *stack_base;  /* for future cleanup */
    uint32_t             tid;
    struct aegis_task_t *next;        /* circular list */
} aegis_task_t;

/* Compile-time guard: ctx_switch.asm assumes rsp is at offset 0 of the TCB.
 * If anyone adds a field before rsp, this catches it immediately. */
_Static_assert(offsetof(aegis_task_t, rsp) == 0,
    "rsp must be first field in aegis_task_t — ctx_switch depends on this");

void  sched_init(void);               /* init run queue, no tasks yet */
void  sched_spawn(void (*fn)(void));  /* alloc TCB + 16KB stack, add to queue */
void  sched_start(void);             /* print [SCHED] OK, then sti — PIT takes over */
void  sched_tick(void);              /* called from pit_handler — advance + switch */
```

`sched_tick()` advances `current = current->next`, calls `ctx_switch(old, current)`.
Single-core only. No locking needed in Phase 4.

**`[SCHED]` OK line ownership**: `sched_start()` owns this print, not `sched_init()`.
`sched_start()` is called after both tasks are spawned, so it can count the run
queue and print the accurate task count before enabling interrupts.

```c
/* main.c call sequence: */
sched_init();
sched_spawn(task_kbd);
sched_spawn(task_heartbeat);
sched_start();   /* prints [SCHED] OK: scheduler started, 2 tasks — then sti */
```

**Phase 4 tasks (spawned in `main.c`):**

```c
/* Task 0: keyboard echo */
static void task_kbd(void) {
    for (;;) {
        char c = kbd_read();
        printk("%c", c);   /* routes through vga_write_string → vga_putchar */
    }
}

/* Task 1: heartbeat — exits after 500 ticks to allow make test to complete */
static void task_heartbeat(void) {
    uint64_t last = 0;
    uint32_t seconds = 0;
    for (;;) {
        uint64_t t = arch_get_ticks();   /* arch-boundary accessor, not g_ticks */
        if (t >= 500) {
            printk("[AEGIS] System halted.\n");
            /* FIXME: arch_debug_exit called from within a scheduled task context.
             * Stack state is indeterminate at this point. Acceptable for Phase 4
             * because isa-debug-exit writes to an I/O port and QEMU exits immediately.
             * Phase 5+ must implement a clean kernel shutdown path. */
            arch_debug_exit(0x01);
        }
        if (t - last >= 100) {
            last = t;
            seconds++;
            /* optional: printk heartbeat to serial for debugging */
        }
    }
}
```

---

## 6. VGA Enhancements

### 6.1 Cursor State

Add to `vga.c`:
```c
static int s_row = 0;
static int s_col = 0;
```

Replace `vga_write_char` with `vga_putchar`:
- Saves and restores the interrupt flag using `pushfq`/`popfq` + `cli` to
  prevent cursor state corruption from preemption mid-write (see note below)
- Writes char at `(s_row, s_col)` with attribute 0x07
- `\n` → col=0, row++
- `\r` → col=0
- `\b` → if col>0: col--, write space at new position
- After each char: if col==80 → col=0, row++
- If row==25: scroll (copy rows 1–24 to rows 0–23 by word loop, clear row 24, row=24)
- Update hardware cursor via ports 0x3D4/0x3D5

```c
void vga_putchar(char c) {
    /* Save and restore RFLAGS.IF rather than blindly issuing cli/sti.
     * This is safe during early boot (no IDT — IF is already 0, popfq
     * restores 0, no sti ever fires), during ISR context (IF already 0),
     * and during normal task execution (IF=1, restored after write). */
    uint64_t flags;
    __asm__ volatile ("pushfq; cli; popq %0" : "=r"(flags) : : "memory");
    /* ... cursor update and write ... */
    __asm__ volatile ("pushq %0; popfq" : : "r"(flags) : "memory");
}
```

`vga_write_string` routes through `vga_putchar` so all output paths share
cursor state. The `pushfq`/`popfq` pattern makes this safe for all callers
including `vga_init()` (called before IDT is installed).

### 6.2 Hardware Cursor

```c
static void vga_update_cursor(void) {
    uint16_t pos = s_row * 80 + s_col;
    outb(0x3D4, 0x0F); outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E); outb(0x3D5, (pos >> 8) & 0xFF);
}
```

`outb` already exists in the project (used by serial/PIC/PIT). Declared in `arch.h`.

---

## 7. Boot Sequence and Test Harness

### 7.1 Expected output (`tests/expected/boot.txt`)

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

### 7.2 Test exit strategy

`task_heartbeat` exits after 500 ticks (~5 seconds). The 10-second timeout in
`run_tests.sh` is sufficient. `arch_debug_exit(0x01)` fires from within the task.

---

## 8. Makefile Changes

```makefile
ARCH_SRCS += kernel/arch/x86_64/idt.c   \
             kernel/arch/x86_64/pic.c   \
             kernel/arch/x86_64/pit.c   \
             kernel/arch/x86_64/kbd.c

ARCH_ASMS += kernel/arch/x86_64/isr.asm        \
             kernel/arch/x86_64/ctx_switch.asm

SCHED_SRCS = kernel/sched/sched.c
```

`SCHED_SRCS` compiled with same CFLAGS as other kernel C. New build dirs:
`build/sched/`.

---

## 9. Arch Boundary Rules

- `outb`/`inb`, port I/O, IRQ numbers, scancode tables: `kernel/arch/x86_64/` only
- `sched.c` calls `ctx_switch` (asm) and `arch_debug_exit` — both declared in `arch.h`
- `sched.c` calls `pmm_alloc_page` for stack allocation — arch-agnostic
- `sched.h` and `sched.c` contain no x86-specific knowledge
- `g_ticks` is a file-static in `pit.c`; `kernel/core/` accesses it only via
  `arch_get_ticks()` declared in `arch.h` — never by including `pit.h` directly
- `vga_putchar` is internal to `vga.c`; `kernel/core/` reaches it via
  `printk("%c", c)` → `vga_write_string` → `vga_putchar`; not in `arch.h`
- `ctx_switch` is declared in `arch.h` and called from `sched.c` — the asm
  implementation is arch-specific but the call site is in the arch-agnostic scheduler

---

## 10. Decisions Log

| Decision | Rationale |
|----------|-----------|
| Interrupt gates (not trap gates) | CPU clears IF on entry — no nested interrupts in Phase 4 |
| 100 Hz PIT | 10ms granularity is fine for kernel tasks; matches standard OS convention |
| Callee-saved-only context switch | Correct for kernel tasks; full frame save deferred to Phase 5 user-space |
| 64-byte kbd ring buffer | Prevents keystrokes from being dropped; 64 chars is more than enough |
| 16KB task stacks (4 pages) | Generous for simple kernel tasks; avoids stack overflow during early development |
| 500-tick test exit | 5 seconds gives scheduler time to prove it works; under the 10s QEMU timeout |
| Shift-only kbd, no extended keys | YAGNI — US QWERTY letters/digits/punctuation sufficient for Phase 4 demo |
| `vga_putchar` replaces `vga_write_char` | Unified output path with cursor state; `printk` and `vga_write_string` route through it |
| EOI sent before handler in `isr_dispatch` | `pit_handler` calls `ctx_switch` which replaces RSP — EOI after handler return is stranded on the outgoing task's return path, silencing future PIT ticks until that task is rescheduled |
| `pushfq`/`popfq` (not `cli`/`sti`) in `vga_putchar` | Saves and restores the actual IF flag: safe during early boot (no IDT, IF=0 — `popfq` restores 0, never fires `sti`), safe in ISR context (IF=0), correct in task context (IF=1 restored). Plain `sti` would triple-fault if called before IDT is installed. |
| `arch_get_ticks()` instead of `g_ticks` extern | PIT tick counter is x86-specific; `kernel/core/` must not reference arch internals directly |
| `sched_start()` owns `[SCHED]` OK print | `sched_init()` initializes an empty queue; only `sched_start()` knows the final task count after all `sched_spawn()` calls |
| Error-code exception list per Intel SDM Vol 3A Table 6-1 | Misclassifying an exception (ISR_ERR vs ISR_NOERR) misaligns the interrupt frame and crashes the handler |
