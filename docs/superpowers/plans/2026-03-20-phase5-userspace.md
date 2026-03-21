# Phase 5 — User Space Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run a statically-linked ELF64 binary in ring 3, printing three messages then exiting, verified by `make test`.

**Architecture:** New GDT/TSS/SYSCALL infrastructure enables ring 3. `aegis_process_t` embeds `aegis_task_t` at offset 0; the scheduler sees only `aegis_task_t *` and uses `is_user` to gate CR3 switches. ELF loaded from a C byte array embedded via `xxd`.

**Tech Stack:** x86-64 C/NASM kernel, `x86_64-elf-gcc`, QEMU, `xxd`

---

## File Structure

**New files:**
- `kernel/arch/x86_64/gdt.h` — GDT selectors + `arch_gdt_init` declaration
- `kernel/arch/x86_64/gdt.c` — runtime GDT with ring-3 descriptors + TSS slot
- `kernel/arch/x86_64/tss.h` — `aegis_tss_t` struct + `arch_tss_get`, `arch_tss_init`, `arch_set_kernel_stack`
- `kernel/arch/x86_64/tss.c` — TSS globals (`s_tss`, `g_kernel_rsp`, `g_user_rsp`), `arch_set_kernel_stack`
- `kernel/arch/x86_64/arch_syscall.c` — `arch_syscall_init` (MSR programming)
- `kernel/arch/x86_64/syscall_entry.asm` — `syscall_entry` stub + `proc_enter_user` label
- `kernel/proc/proc.h` — `aegis_process_t`, `proc_spawn` declaration
- `kernel/proc/proc.c` — `proc_spawn` implementation
- `kernel/elf/elf.h` — `elf_load` declaration
- `kernel/elf/elf.c` — ELF64 static loader
- `kernel/syscall/syscall.h` — `syscall_dispatch` declaration
- `kernel/syscall/syscall.c` — `write` (1) and `exit` (60) handlers
- `user/init/main.c` — freestanding user binary
- `user/init/Makefile` — builds `init.elf`

**Modified files:**
- `kernel/arch/x86_64/arch.h` — add Phase 5 arch declarations
- `kernel/sched/sched.h` — add `kernel_stack_top`, `is_user` to `aegis_task_t`; add `sched_add`, `sched_exit`
- `kernel/sched/sched.c` — update `sched_spawn`; add `sched_add`, `sched_exit`; update `sched_tick`, `sched_start`
- `kernel/mm/vmm.h` — add `vmm_create_user_pml4`, `vmm_map_user_page`, `vmm_switch_to`
- `kernel/mm/vmm.c` — implement above
- `kernel/core/main.c` — add gdt/tss/syscall init; add `proc_spawn` call
- `Makefile` — new dirs, CFLAGS -I paths, user binary build chain
- `tests/expected/boot.txt` — add GDT/TSS/SYSCALL/USER/updated SCHED lines

---

### Task 1: RED — Update boot.txt

**Files:**
- Modify: `tests/expected/boot.txt`

- [ ] **Step 1: Update boot.txt with Phase 5 expected output**

Replace the file content with:
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
[GDT] OK: ring 3 descriptors installed
[TSS] OK: RSP0 initialized
[SYSCALL] OK: SYSCALL/SYSRET enabled
[SCHED] OK: scheduler started, 3 tasks
[USER] hello from ring 3
[USER] hello from ring 3
[USER] hello from ring 3
[USER] done
[AEGIS] System halted.
```

- [ ] **Step 2: Verify test now fails**

```bash
make test 2>&1 | tail -20
```
Expected: FAIL (diff shows missing GDT/TSS/SYSCALL/USER lines)

- [ ] **Step 3: Commit**

```bash
git add tests/expected/boot.txt
git commit -m "test: RED for Phase 5 user space — add GDT/TSS/SYSCALL/USER lines to boot.txt"
```

---

### Task 2: User Binary

**Files:**
- Create: `user/init/main.c`
- Create: `user/init/Makefile`

- [ ] **Step 1: Create `user/init/` directory and `main.c`**

```bash
mkdir -p user/init
```

`user/init/main.c`:
```c
/* Freestanding user-space init binary — no libc, no headers */

static inline long
sys_write(int fd, const char *buf, long len)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(1L), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return ret;
}

static inline void
sys_exit(int code)
{
    __asm__ volatile ("syscall"
        : : "a"(60L), "D"((long)code) : "rcx", "r11");
    __builtin_unreachable();
}

void
_start(void)
{
    int i;
    for (i = 0; i < 3; i++)
        sys_write(1, "[USER] hello from ring 3\n", 25);
    sys_write(1, "[USER] done\n", 12);
    sys_exit(0);
}
```

- [ ] **Step 2: Create `user/init/Makefile`**

```makefile
CC = x86_64-elf-gcc

.PHONY: all clean

all: init.elf

init.elf: main.c
	$(CC) -ffreestanding -nostdlib -nostdinc -static \
	    -Wl,-Ttext=0x400000 -Wl,-e,_start \
	    -o init.elf main.c

clean:
	rm -f init.elf init_bin.c
```

- [ ] **Step 3: Build and verify ELF**

```bash
make -C user/init
file user/init/init.elf
```
Expected: `ELF 64-bit LSB executable, x86-64, statically linked`

- [ ] **Step 4: Commit**

```bash
git add user/init/main.c user/init/Makefile
git commit -m "feat: add freestanding user init binary (ring 3 test)"
```

---

### Task 3: TSS Header and Implementation

**Files:**
- Create: `kernel/arch/x86_64/tss.h`
- Create: `kernel/arch/x86_64/tss.c`

- [ ] **Step 1: Create `kernel/arch/x86_64/tss.h`**

```c
#ifndef ARCH_TSS_H
#define ARCH_TSS_H

#include <stdint.h>

/*
 * 104-byte x86-64 Task State Segment.
 * Only RSP0 is used — loaded by CPU on ring 3→0 transitions.
 * iomap_base = 104 disables I/O permission bitmap.
 */
typedef struct {
    uint32_t reserved0;    /* +0   */
    uint64_t rsp0;         /* +4   — kernel stack for ring 3 interrupts */
    uint64_t rsp1;         /* +12  */
    uint64_t rsp2;         /* +20  */
    uint64_t reserved1;    /* +28  */
    uint64_t ist[7];       /* +36  — IST1-IST7, all zero */
    uint64_t reserved2;    /* +92  */
    uint16_t reserved3;    /* +100 */
    uint16_t iomap_base;   /* +102 — 104 = disable I/O bitmap */
} __attribute__((packed)) aegis_tss_t;

/* Returns pointer to the static TSS (used by gdt.c to install TSS descriptor). */
aegis_tss_t *arch_tss_get(void);

/* Initialize TSS fields; set iomap_base = 104. Prints [TSS] OK. */
void arch_tss_init(void);

/* Update TSS.RSP0 and g_kernel_rsp to rsp0.
 * Called by scheduler before every ctx_switch so the CPU uses the
 * correct kernel stack top when the next ring-3 interrupt fires. */
void arch_set_kernel_stack(uint64_t rsp0);

#endif /* ARCH_TSS_H */
```

- [ ] **Step 2: Create `kernel/arch/x86_64/tss.c`**

```c
#include "tss.h"
#include "printk.h"

/* Static TSS — zeroed by GRUB ELF loader (BSS). */
static aegis_tss_t s_tss;

/*
 * g_kernel_rsp — kernel stack RSP for SYSCALL entry stub.
 * Updated by arch_set_kernel_stack() alongside tss.rsp0.
 * Exported (no static) so syscall_entry.asm can reference it.
 *
 * g_user_rsp — scratch global for SYSCALL stack switch.
 * Stores user RSP transiently between "mov [g_user_rsp], rsp"
 * and the push to the kernel stack. Single-core only.
 */
uint64_t g_kernel_rsp = 0;
uint64_t g_user_rsp   = 0;

aegis_tss_t *
arch_tss_get(void)
{
    return &s_tss;
}

void
arch_tss_init(void)
{
    s_tss.iomap_base = 104;   /* offset past end of TSS → I/O bitmap disabled */
    printk("[TSS] OK: RSP0 initialized\n");
}

void
arch_set_kernel_stack(uint64_t rsp0)
{
    s_tss.rsp0   = rsp0;
    g_kernel_rsp = rsp0;
}
```

- [ ] **Step 3: Verify tss.c compiles in isolation**

```bash
make 2>&1 | grep -E "tss\.c|error:" | head -20
```
Expected: no errors (tss.c not yet in Makefile — that's Task 12; this step is just mental verification of the code)

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/tss.h kernel/arch/x86_64/tss.c
git commit -m "feat: add TSS struct, arch_tss_init, arch_set_kernel_stack"
```

---

### Task 4: GDT with Ring-3 Descriptors

**Files:**
- Create: `kernel/arch/x86_64/gdt.h`
- Create: `kernel/arch/x86_64/gdt.c`

- [ ] **Step 1: Create `kernel/arch/x86_64/gdt.h`**

```c
#ifndef ARCH_GDT_H
#define ARCH_GDT_H

/*
 * GDT segment selectors.
 *
 * CRITICAL: user data (0x18) MUST precede user code (0x20).
 * SYSRET derives selectors arithmetically from STAR[63:48]=0x10:
 *   SS = (0x10 + 8)  | 3 = 0x1B → GDT[3] = user data  ✓
 *   CS = (0x10 + 16) | 3 = 0x23 → GDT[4] = user code  ✓
 * Swapping user data/code causes SYSRET to load a data descriptor
 * into CS, triggering an immediate #GP on return to ring 3.
 */
#define GDT_SEL_NULL         0x00
#define GDT_SEL_KERNEL_CODE  0x08
#define GDT_SEL_KERNEL_DATA  0x10
#define GDT_SEL_USER_DATA    0x18   /* index 3 — MUST be before user code */
#define GDT_SEL_USER_CODE    0x20   /* index 4 */
#define GDT_SEL_TSS          0x28   /* 16-byte system descriptor at indices 5+6 */

/*
 * User selectors with RPL=3 (for iretq frames and SYSCALL/SYSRET).
 * CS=0x23, SS=0x1B.
 */
#define GDT_USER_CODE_RPL3   (GDT_SEL_USER_CODE | 3)
#define GDT_USER_DATA_RPL3   (GDT_SEL_USER_DATA | 3)

/* Build runtime GDT, lgdt, reload segments, ltr.  Prints [GDT] OK. */
void arch_gdt_init(void);

#endif /* ARCH_GDT_H */
```

- [ ] **Step 2: Create `kernel/arch/x86_64/gdt.c`**

```c
#include "gdt.h"
#include "tss.h"
#include "printk.h"
#include <stdint.h>

/* 7 slots: null + kernel code + kernel data + user data + user code + TSS(low+high) */
#define GDT_ENTRIES 7

typedef struct {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mi;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_hi;
} __attribute__((packed)) gdt_desc_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed, aligned(2))) gdtr_t;

static gdt_desc_t s_gdt[GDT_ENTRIES];

/*
 * 64-bit code descriptor: L=1 (long mode), D=0.
 * access = 0x9A = P|DPL<<5|S|Type(execute/read). DPL in bits 6:5.
 * gran   = 0x20 = L=1.
 */
static gdt_desc_t
make_code64(int dpl)
{
    gdt_desc_t d = {0};
    d.access = (uint8_t)(0x9A | ((dpl & 3) << 5));
    d.gran   = 0x20;
    return d;
}

/*
 * 64-bit data descriptor.
 * access = 0x92 = P|DPL<<5|S|Type(read/write).
 * gran   = 0x00 (no L bit for data).
 */
static gdt_desc_t
make_data(int dpl)
{
    gdt_desc_t d = {0};
    d.access = (uint8_t)(0x92 | ((dpl & 3) << 5));
    return d;
}

/*
 * Install a 16-byte 64-bit TSS descriptor at GDT indices [idx, idx+1].
 * type=0x89: Present, DPL=0, system descriptor (S=0), Type=9 (64-bit TSS avail).
 */
static void
gdt_set_tss(int idx, uint64_t base, uint32_t limit)
{
    s_gdt[idx].limit_lo = (uint16_t)(limit & 0xFFFF);
    s_gdt[idx].base_lo  = (uint16_t)(base & 0xFFFF);
    s_gdt[idx].base_mi  = (uint8_t)((base >> 16) & 0xFF);
    s_gdt[idx].access   = 0x89;
    s_gdt[idx].gran     = (uint8_t)((limit >> 16) & 0x0F);
    s_gdt[idx].base_hi  = (uint8_t)((base >> 24) & 0xFF);
    /* Second 8 bytes: upper 32 bits of base, rest zeroed */
    uint32_t *hi = (uint32_t *)&s_gdt[idx + 1];
    hi[0] = (uint32_t)(base >> 32);
    hi[1] = 0;
}

void
arch_gdt_init(void)
{
    s_gdt[0] = (gdt_desc_t){0};          /* null */
    s_gdt[1] = make_code64(0);           /* 0x08 kernel code DPL=0 */
    s_gdt[2] = make_data(0);             /* 0x10 kernel data DPL=0 */
    s_gdt[3] = make_data(3);             /* 0x18 user data  DPL=3 — MUST be index 3 */
    s_gdt[4] = make_code64(3);           /* 0x20 user code  DPL=3 — MUST be index 4 */

    aegis_tss_t *tss = arch_tss_get();
    gdt_set_tss(5, (uint64_t)(uintptr_t)tss, sizeof(aegis_tss_t) - 1);

    gdtr_t gdtr;
    gdtr.limit = (uint16_t)(sizeof(s_gdt) - 1);
    gdtr.base  = (uint64_t)(uintptr_t)s_gdt;

    __asm__ volatile (
        "lgdt %0\n\t"
        /* Far return to reload CS=0x08 */
        "pushq $0x08\n\t"
        "leaq  1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        /* Reload data segment registers to kernel data (0x10) */
        "movw $0x10, %%ax\n\t"
        "movw %%ax,  %%ds\n\t"
        "movw %%ax,  %%es\n\t"
        "movw %%ax,  %%fs\n\t"
        "movw %%ax,  %%gs\n\t"
        "movw %%ax,  %%ss\n\t"
        /* Load TSS selector */
        "movw $0x28, %%ax\n\t"
        "ltr  %%ax\n\t"
        : : "m"(gdtr) : "rax", "memory"
    );

    printk("[GDT] OK: ring 3 descriptors installed\n");
}
```

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/gdt.h kernel/arch/x86_64/gdt.c
git commit -m "feat: add runtime GDT with ring-3 descriptors and TSS slot"
```

---

### Task 5: SYSCALL Entry Stub

**Files:**
- Create: `kernel/arch/x86_64/syscall_entry.asm`
- Create: `kernel/arch/x86_64/arch_syscall.c`

- [ ] **Step 1: Create `kernel/arch/x86_64/syscall_entry.asm`**

```nasm
; syscall_entry.asm — SYSCALL landing pad and ring-3 entry helper
;
; syscall_entry: called by CPU on SYSCALL instruction.
;   CPU state on entry:
;     RCX = return RIP (user), R11 = saved RFLAGS, RSP = user RSP
;     IF=0, DF=0 (set by IA32_SFMASK=0x700)
;
; proc_enter_user: bare iretq label used by proc_spawn to enter ring 3
;   for the first time. ctx_switch's ret lands here; RSP points at an
;   iretq frame built by proc_spawn. Must NOT have a C prologue.

bits 64
section .text

extern syscall_dispatch
extern g_kernel_rsp
extern g_user_rsp

global syscall_entry
global proc_enter_user

syscall_entry:
    ; RSP is still user RSP — save it, switch to kernel stack
    mov  [rel g_user_rsp], rsp
    mov  rsp, [rel g_kernel_rsp]

    ; Build restore frame on kernel stack (popped before sysretq)
    push qword [rel g_user_rsp]   ; [rsp+16] user RSP  (deepest)
    push rcx                       ; [rsp+8]  return RIP
    push r11                       ; [rsp+0]  RFLAGS    (top, popped first)

    ; Translate Linux syscall ABI → SysV C calling convention:
    ;   Linux: rax=num, rdi=arg1, rsi=arg2, rdx=arg3
    ;   SysV:  rdi=num, rsi=arg1, rdx=arg2, rcx=arg3
    ; rcx already pushed above (return RIP), safe to overwrite now.
    mov  rcx, rdx    ; arg3 ← user rdx
    mov  rdx, rsi    ; arg2 ← user rsi
    mov  rsi, rdi    ; arg1 ← user rdi
    mov  rdi, rax    ; num  ← syscall number

    call syscall_dispatch
    ; rax = return value (already in position for sysretq)

    pop  r11          ; restore RFLAGS
    pop  rcx          ; restore return RIP
    pop  rsp          ; restore user RSP (pop rsp sets RSP=[RSP], not RSP+8)

    sysretq

; proc_enter_user — bare iretq, NO C prologue.
; On entry RSP points at ring-3 iretq frame:
;   [rsp+0]  RIP  (user entry point)
;   [rsp+8]  CS   (0x23 = user code | RPL=3)
;   [rsp+16] RFLAGS (0x202 = IF=1 | reserved bit 1)
;   [rsp+24] RSP  (user stack top, 16-byte aligned)
;   [rsp+32] SS   (0x1B = user data | RPL=3)
proc_enter_user:
    iretq
```

- [ ] **Step 2: Create `kernel/arch/x86_64/arch_syscall.c`**

```c
#include "printk.h"
#include <stdint.h>

#define IA32_EFER   0xC0000080UL
#define IA32_STAR   0xC0000081UL
#define IA32_LSTAR  0xC0000082UL
#define IA32_SFMASK 0xC0000084UL

static inline void
wrmsr(uint32_t msr, uint64_t val)
{
    __asm__ volatile ("wrmsr"
        : : "c"(msr),
            "a"((uint32_t)(val & 0xFFFFFFFFUL)),
            "d"((uint32_t)(val >> 32))
        : "memory");
}

static inline uint64_t
rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* Defined in syscall_entry.asm */
extern void syscall_entry(void);

void
arch_syscall_init(void)
{
    /* Enable SYSCALL/SYSRET: set SCE bit (bit 0) in IA32_EFER */
    wrmsr(IA32_EFER, rdmsr(IA32_EFER) | 1UL);

    /*
     * STAR selector layout:
     *   bits [47:32] = 0x0008  → SYSCALL: CS=0x08 (kernel code), SS=0x10 (kernel data)
     *   bits [63:48] = 0x0010  → SYSRET:  SS=(0x10+8)|3=0x1B, CS=(0x10+16)|3=0x23
     *
     * The user data descriptor (0x18) must be at GDT index 3 and user code (0x20)
     * at GDT index 4 for these selector values to land on the correct descriptors.
     */
    wrmsr(IA32_STAR, (0x0010ULL << 48) | (0x0008ULL << 32));

    /* LSTAR: syscall entry point */
    wrmsr(IA32_LSTAR, (uint64_t)(uintptr_t)syscall_entry);

    /*
     * SFMASK: bits cleared in RFLAGS on SYSCALL entry.
     *   bit 9 (IF)  — disable interrupts; syscall dispatch is non-preemptible
     *   bit 8 (TF)  — disable single-step traps
     *   bit 10 (DF) — clear direction flag; required for kernel C string ops
     *   0x700 = bits 10:8
     */
    wrmsr(IA32_SFMASK, 0x700UL);

    printk("[SYSCALL] OK: SYSCALL/SYSRET enabled\n");
}
```

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/syscall_entry.asm kernel/arch/x86_64/arch_syscall.c
git commit -m "feat: add SYSCALL entry stub and arch_syscall_init MSR programming"
```

---

### Task 6: Update arch.h with Phase 5 Declarations

**Files:**
- Modify: `kernel/arch/x86_64/arch.h`

- [ ] **Step 1: Add Phase 5 section to arch.h**

After the `/* Phase 4: Keyboard */` section, add:

```c
/* -------------------------------------------------------------------------
 * Phase 5: Ring-3 support
 * ------------------------------------------------------------------------- */

/* Build a 7-entry runtime GDT (null, kernel code/data, user data/code, TSS),
 * install with lgdt, reload segment registers, load TSS with ltr.
 * Prints [GDT] OK. Must be called after arch_tss_init context is set up
 * (TSS struct is BSS-zeroed; gdt_init uses arch_tss_get() for TSS base). */
void arch_gdt_init(void);

/* Set TSS.iomap_base = 104 (disables I/O permission bitmap).
 * Prints [TSS] OK. */
void arch_tss_init(void);

/* Update both TSS.RSP0 and g_kernel_rsp to rsp0.
 * Called by scheduler before every ctx_switch so:
 *   — CPU loads correct kernel stack top on ring-3 interrupts (via TSS.RSP0)
 *   — syscall_entry.asm loads correct kernel stack (via g_kernel_rsp)
 * Both values must always be identical. */
void arch_set_kernel_stack(uint64_t rsp0);

/* Program IA32_EFER (SCE), IA32_STAR, IA32_LSTAR, IA32_SFMASK.
 * Prints [SYSCALL] OK. */
void arch_syscall_init(void);
```

- [ ] **Step 2: Verify arch.h compiles (included by main.c)**

```bash
make 2>&1 | grep "arch.h" | head -10
```
Expected: no parse errors from arch.h

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/arch.h
git commit -m "feat: add Phase 5 arch declarations to arch.h"
```

---

### Task 7: Extend sched.h and Update sched.c

**Files:**
- Modify: `kernel/sched/sched.h`
- Modify: `kernel/sched/sched.c`

- [ ] **Step 1: Update `aegis_task_t` in sched.h**

Replace the struct with:
```c
typedef struct aegis_task_t {
    uint64_t             rsp;              /* MUST be first — ctx_switch reads [rdi+0] */
    uint8_t             *stack_base;       /* bottom of PMM-allocated stack (for future cleanup) */
    uint64_t             kernel_stack_top; /* RSP0 value: kernel stack top for this task */
    uint32_t             tid;              /* task ID */
    uint8_t              is_user;          /* 1 = user process (aegis_process_t), 0 = kernel task */
    struct aegis_task_t *next;             /* circular linked list */
} aegis_task_t;
```

Add new declarations after `sched_tick`:
```c
/* Add a pre-initialized TCB to the run queue.
 * Used by proc_spawn to insert a user process without duplicating
 * the list-insertion logic from sched_spawn. */
void sched_add(aegis_task_t *task);

/* Remove the current task from the run queue and switch to the next task.
 * Called from syscall exit (60). Does not return. Memory is leaked
 * (intentional for Phase 5 — Phase 6 adds cleanup). */
void sched_exit(void);
```

- [ ] **Step 2: Update `sched_spawn` in sched.c to set new fields**

In `sched_spawn`, after the line `task->stack_base = stack;` and before `task->tid = s_next_tid++`, add:
```c
    task->kernel_stack_top = (uint64_t)(uintptr_t)(stack + STACK_SIZE);
    task->is_user          = 0;
```

- [ ] **Step 3: Add `sched_add` implementation in sched.c**

After `sched_spawn`, add:
```c
void
sched_add(aegis_task_t *task)
{
    if (!s_current) {
        task->next = task;
        s_current  = task;
    } else {
        task->next      = s_current->next;
        s_current->next = task;
    }
    s_task_count++;
}
```

- [ ] **Step 4: Add `sched_exit` implementation in sched.c**

Add after `sched_add`. Note: this requires `proc.h` and `vmm.h` includes (added in Step 5):

```c
void
sched_exit(void)
{
    /* IF=0 throughout (IA32_SFMASK cleared IF on SYSCALL entry) —
     * no preemption can occur during list manipulation. */
    aegis_task_t *prev = s_current;
    while (prev->next != s_current)
        prev = prev->next;

    aegis_task_t *dying = s_current;
    s_current           = dying->next;
    prev->next          = s_current;
    s_task_count--;

    if (s_current == dying) {  /* last task — everything has exited */
        arch_request_shutdown();
        for (;;) __asm__ volatile ("hlt");
    }

    arch_set_kernel_stack(s_current->kernel_stack_top);
    if (s_current->is_user)
        vmm_switch_to(((aegis_process_t *)s_current)->pml4_phys);

    /*
     * PHASE 6 CLEANUP NOTE: ctx_switch saves dying->rsp here — that RSP
     * is somewhere in the middle of the kernel stack (the call depth at
     * sched_exit time). Phase 6 must free dying->stack_base (the PMM
     * allocation) and the process TCB using the physical addresses, not
     * by dereferencing dying->rsp. dying->stack_base + STACK_SIZE gives
     * the allocation top; dying->rsp is the current stack pointer.
     */
    ctx_switch(dying, s_current);
    __builtin_unreachable();
}
```

- [ ] **Step 5: Update includes and `sched_tick` / `sched_start` in sched.c**

Add to the includes at the top of sched.c:
```c
#include "vmm.h"
#include "proc.h"
```

Update `sched_tick` to add arch_set_kernel_stack + vmm_switch_to before ctx_switch:
```c
void
sched_tick(void)
{
    if (!s_current)
        return;
    if (s_current->next == s_current)
        return;

    aegis_task_t *old = s_current;
    s_current = s_current->next;

    arch_set_kernel_stack(s_current->kernel_stack_top);
    if (s_current->is_user)
        vmm_switch_to(((aegis_process_t *)s_current)->pml4_phys);

    ctx_switch(old, s_current);
}
```

Update `sched_start` to add arch_set_kernel_stack + vmm_switch_to before ctx_switch:
```c
void
sched_start(void)
{
    if (!s_current) {
        printk("[SCHED] FAIL: sched_start called with no tasks\n");
        for (;;) {}
    }

    printk("[SCHED] OK: scheduler started, %u tasks\n", s_task_count);

    arch_set_kernel_stack(s_current->kernel_stack_top);
    if (s_current->is_user)
        vmm_switch_to(((aegis_process_t *)s_current)->pml4_phys);

    aegis_task_t dummy;
    ctx_switch(&dummy, s_current);
    __builtin_unreachable();
}
```

- [ ] **Step 6: Commit**

```bash
git add kernel/sched/sched.h kernel/sched/sched.c
git commit -m "feat: extend aegis_task_t with kernel_stack_top/is_user; add sched_add, sched_exit"
```

---

### Task 8: VMM Extensions

**Files:**
- Modify: `kernel/mm/vmm.h`
- Modify: `kernel/mm/vmm.c`

- [ ] **Step 1: Add new declarations to vmm.h**

Add after `vmm_unmap_page`:

```c
/* Allocate a new PML4 and copy kernel high entries [256..511] from the
 * master PML4. Returns physical address of the new PML4.
 * Valid while identity map [0..4MB) is active (Phase 5 constraint). */
uint64_t vmm_create_user_pml4(void);

/* Map a single 4KB page in pml4_phys (NOT the active kernel PML4).
 * All intermediate page-table entries (PML4e, PDPTe, PDe) created for
 * this mapping have VMM_FLAG_USER set — required because the x86-64 MMU
 * checks the USER bit at every level of the page-table walk.
 * A leaf PTE with USER set but any ancestor without USER causes a ring-3
 * #PF even if the leaf mapping is correct.
 * flags: VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE as needed. */
void vmm_map_user_page(uint64_t pml4_phys, uint64_t virt,
                       uint64_t phys, uint64_t flags);

/* Load pml4_phys into CR3. Flushes TLB. */
void vmm_switch_to(uint64_t pml4_phys);
```

- [ ] **Step 2: Add implementations to vmm.c**

Add a `ensure_table_user` helper and the three new functions after `vmm_unmap_page`:

```c
/*
 * ensure_table_user — like ensure_table but sets USER|PRESENT|WRITABLE
 * on newly created intermediate entries.
 * CRITICAL: ALL intermediate entries in a user page-table walk must have
 * VMM_FLAG_USER set. The MMU checks the USER bit at every level (PML4e,
 * PDPTe, PDe). A missing USER bit on any ancestor causes a ring-3 #PF
 * even if the leaf PTE is correct.
 */
static uint64_t
ensure_table_user(uint64_t *parent, uint64_t idx)
{
    if (!(parent[idx] & VMM_FLAG_PRESENT)) {
        uint64_t child = alloc_table();
        parent[idx] = child | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;
    }
    return PTE_ADDR(parent[idx]);
}

uint64_t
vmm_create_user_pml4(void)
{
    uint64_t new_pml4_phys = alloc_table();   /* zeroed by alloc_table */

    /* Copy kernel high entries [256..511] from master PML4.
     * This makes the kernel higher-half accessible in every user process's
     * address space, so syscall handlers can execute after SYSCALL without
     * a CR3 switch. */
    uint64_t *master = phys_to_table(s_pml4_phys);
    uint64_t *newpml = phys_to_table(new_pml4_phys);
    int i;
    for (i = 256; i < 512; i++)
        newpml[i] = master[i];

    return new_pml4_phys;
}

void
vmm_map_user_page(uint64_t pml4_phys, uint64_t virt,
                  uint64_t phys, uint64_t flags)
{
    if (virt & ~VMM_PAGE_MASK) {
        printk("[VMM] FAIL: vmm_map_user_page virt not aligned\n");
        for (;;) {}
    }
    if (phys & ~VMM_PAGE_MASK) {
        printk("[VMM] FAIL: vmm_map_user_page phys not aligned\n");
        for (;;) {}
    }

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t *pml4    = phys_to_table(pml4_phys);
    uint64_t pdpt_p   = ensure_table_user(pml4, pml4_idx);
    uint64_t *pdpt    = phys_to_table(pdpt_p);
    uint64_t pd_p     = ensure_table_user(pdpt, pdpt_idx);
    uint64_t *pd      = phys_to_table(pd_p);
    uint64_t pt_p     = ensure_table_user(pd, pd_idx);
    uint64_t *pt      = phys_to_table(pt_p);

    if (pt[pt_idx] & VMM_FLAG_PRESENT) {
        printk("[VMM] FAIL: vmm_map_user_page double-map\n");
        for (;;) {}
    }

    pt[pt_idx] = phys | flags | VMM_FLAG_PRESENT;
}

void
vmm_switch_to(uint64_t pml4_phys)
{
    arch_vmm_load_pml4(pml4_phys);
}
```

- [ ] **Step 3: Commit**

```bash
git add kernel/mm/vmm.h kernel/mm/vmm.c
git commit -m "feat: add vmm_create_user_pml4, vmm_map_user_page, vmm_switch_to"
```

---

### Task 9: ELF Loader

**Files:**
- Create: `kernel/elf/elf.h`
- Create: `kernel/elf/elf.c`

- [ ] **Step 1: Create `kernel/elf/elf.h`**

```c
#ifndef AEGIS_ELF_H
#define AEGIS_ELF_H

#include <stdint.h>
#include <stddef.h>

/* Load a static ELF64 into pml4_phys's address space.
 * Maps all PT_LOAD segments; allocates PMM pages.
 * Returns e_entry (virtual entry point) on success, 0 on parse error.
 * Panics if PMM is exhausted. */
uint64_t elf_load(uint64_t pml4_phys, const uint8_t *data, size_t len);

#endif /* AEGIS_ELF_H */
```

- [ ] **Step 2: Create `kernel/elf/elf.c`**

```c
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "printk.h"
#include <stdint.h>
#include <stddef.h>

/* ELF64 header */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

/* ELF64 program header */
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

#define PT_LOAD    1
#define PF_W       2      /* program header write flag */
#define ELFCLASS64 2
#define ET_EXEC    2
#define EM_X86_64  0x3E

uint64_t
elf_load(uint64_t pml4_phys, const uint8_t *data, size_t len)
{
    (void)len;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;

    /* Verify ELF magic */
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        printk("[ELF] FAIL: bad magic\n");
        return 0;
    }
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_type != ET_EXEC ||
        eh->e_machine != EM_X86_64) {
        printk("[ELF] FAIL: not a static ELF64 x86-64 executable\n");
        return 0;
    }

    const Elf64_Phdr *phdrs = (const Elf64_Phdr *)(data + eh->e_phoff);
    uint16_t i;
    for (i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD)
            continue;

        /* Allocate physically contiguous pages for this segment.
         *
         * CONTIGUITY ASSUMPTION: The Phase 3 bitmap PMM allocates sequentially,
         * so successive pmm_alloc_page() calls return physically adjacent frames.
         * This allows treating the pages as a single region for memcpy.
         * If the PMM becomes non-sequential (Phase 6+ buddy allocator),
         * replace this with a page-by-page copy. Same assumption as sched_spawn. */
        uint64_t page_count = (ph->p_memsz + 4095UL) / 4096UL;
        uint64_t first_phys = 0;
        uint64_t j;
        for (j = 0; j < page_count; j++) {
            uint64_t p = pmm_alloc_page();
            if (!p) {
                printk("[ELF] FAIL: OOM loading segment\n");
                for (;;) {}
            }
            if (j == 0)
                first_phys = p;
        }

        /* Copy file bytes into physical memory */
        uint8_t *dst = (uint8_t *)(uintptr_t)first_phys;
        const uint8_t *src = data + ph->p_offset;
        uint64_t k;
        for (k = 0; k < ph->p_filesz; k++)
            dst[k] = src[k];

        /* Zero BSS (bytes past p_filesz up to p_memsz) */
        for (k = ph->p_filesz; k < ph->p_memsz; k++)
            dst[k] = 0;

        /* Map each page into the user address space */
        uint64_t map_flags = VMM_FLAG_USER;
        if (ph->p_flags & PF_W)
            map_flags |= VMM_FLAG_WRITABLE;

        for (j = 0; j < page_count; j++) {
            vmm_map_user_page(pml4_phys,
                              ph->p_vaddr + j * 4096UL,
                              first_phys  + j * 4096UL,
                              map_flags);
        }
    }

    return eh->e_entry;
}
```

- [ ] **Step 3: Commit**

```bash
git add kernel/elf/elf.h kernel/elf/elf.c
git commit -m "feat: add ELF64 static loader (PT_LOAD segments, user page mapping)"
```

---

### Task 10: Syscall Dispatch

**Files:**
- Create: `kernel/syscall/syscall.h`
- Create: `kernel/syscall/syscall.c`

- [ ] **Step 1: Create `kernel/syscall/syscall.h`**

```c
#ifndef AEGIS_SYSCALL_H
#define AEGIS_SYSCALL_H

#include <stdint.h>

/* Called from syscall_entry.asm with IF=0, DF=0, on kernel stack.
 * Returns value placed in RAX on syscall return. */
uint64_t syscall_dispatch(uint64_t num, uint64_t arg1,
                          uint64_t arg2, uint64_t arg3);

#endif /* AEGIS_SYSCALL_H */
```

- [ ] **Step 2: Create `kernel/syscall/syscall.c`**

```c
#include "syscall.h"
#include "sched.h"
#include "printk.h"
#include <stdint.h>

/*
 * sys_write — syscall 1
 *
 * arg1 = fd (ignored: all output goes to printk)
 * arg2 = user virtual address of buffer
 * arg3 = byte count
 *
 * Prints via printk character-by-character because printk does not
 * support %.*s.  Direct dereference of arg2 is safe: SMAP is not
 * enabled and the user PML4 shares the kernel higher-half, so the
 * kernel can read user addresses while the user PML4 is loaded in CR3.
 *
 * PHASE 5 SECURITY DEBT: no pointer validation. A malicious user
 * could pass a kernel-space address as arg2 and read arbitrary kernel
 * memory via printk. Phase 6 must add:
 *   (a) bounds check: arg2 + arg3 <= 0x00007FFFFFFFFFFF
 *   (b) SMAP enable so unintentional kernel→user dereferences fault.
 */
static uint64_t
sys_write(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    (void)arg1;
    const char *s = (const char *)(uintptr_t)arg2;
    uint64_t i;
    for (i = 0; i < arg3; i++)
        printk("%c", s[i]);
    return arg3;
}

/*
 * sys_exit — syscall 60
 *
 * arg1 = exit code (ignored for Phase 5)
 * Calls sched_exit() which never returns.
 */
static uint64_t
sys_exit(uint64_t arg1)
{
    (void)arg1;
    sched_exit();
    __builtin_unreachable();
}

uint64_t
syscall_dispatch(uint64_t num, uint64_t arg1,
                 uint64_t arg2, uint64_t arg3)
{
    switch (num) {
    case 1:  return sys_write(arg1, arg2, arg3);
    case 60: return sys_exit(arg1);
    default: return (uint64_t)-1;   /* ENOSYS */
    }
}
```

- [ ] **Step 3: Commit**

```bash
git add kernel/syscall/syscall.h kernel/syscall/syscall.c
git commit -m "feat: add syscall dispatch with write(1) and exit(60)"
```

---

### Task 11: Process Type

**Files:**
- Create: `kernel/proc/proc.h`
- Create: `kernel/proc/proc.c`

- [ ] **Step 1: Create `kernel/proc/proc.h`**

```c
#ifndef AEGIS_PROC_H
#define AEGIS_PROC_H

#include "sched.h"
#include <stdint.h>
#include <stddef.h>

/*
 * aegis_process_t — user process control block.
 *
 * task MUST be the first member: the scheduler stores all tasks as
 * aegis_task_t * pointers. When task.is_user == 1, the scheduler casts
 * the pointer to aegis_process_t * to reach pml4_phys. This cast is
 * safe only because task is at offset 0 (guaranteed by this layout).
 * Never use this cast when is_user == 0 — kernel tasks are not
 * aegis_process_t instances and the memory past the aegis_task_t fields
 * is unrelated.
 */
typedef struct {
    aegis_task_t task;      /* MUST be first — scheduler casts to aegis_task_t * */
    uint64_t     pml4_phys; /* physical address of this process's PML4 */
} aegis_process_t;

/* Load elf_data (embedded via xxd) into a new user process and add it
 * to the scheduler run queue. Called from kernel_main before sched_start. */
void proc_spawn(const uint8_t *elf_data, size_t elf_len);

#endif /* AEGIS_PROC_H */
```

- [ ] **Step 2: Create `kernel/proc/proc.c`**

```c
#include "proc.h"
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "arch.h"
#include "printk.h"
#include <stdint.h>
#include <stddef.h>

/*
 * init_elf / init_elf_len — generated by:
 *   cd user/init && xxd -i init.elf > init_bin.c
 * which produces exactly these symbol names when invoked as "xxd -i init.elf".
 */
extern const unsigned char init_elf[];
extern const unsigned int  init_elf_len;

/* 16KB kernel stack for the user process (4 pages, contiguous via PMM assumption) */
#define STACK_PAGES 4
#define STACK_SIZE  (STACK_PAGES * 4096UL)

/*
 * User stack layout:
 *   top = USER_STACK_TOP  (= 0x7FFFFFFF000, 16-byte aligned)
 *   page = [USER_STACK_PAGE, USER_STACK_TOP)
 * 0x7FFFFFFF000 ends in 0x000 — 4096-byte aligned → 16-byte aligned. ✓
 * Per AMD64 ABI, RSP on entry to _start must be 16-byte aligned.
 */
#define USER_STACK_TOP  0x7FFFFFFF000ULL
#define USER_STACK_PAGE (USER_STACK_TOP - 4096ULL)

/* Exported from syscall_entry.asm — a bare iretq, NOT a C function.
 * proc_spawn uses its address only as a return-address slot in the
 * initial kernel stack frame. */
extern void proc_enter_user(void);

void
proc_spawn(const uint8_t *elf_data, size_t elf_len)
{
    /* Allocate process control block (one PMM page) */
    uint64_t tcb_phys = pmm_alloc_page();
    if (!tcb_phys) {
        printk("[PROC] FAIL: OOM allocating process TCB\n");
        for (;;) {}
    }
    aegis_process_t *proc = (aegis_process_t *)(uintptr_t)tcb_phys;

    /* Allocate kernel stack (STACK_PAGES contiguous pages — see PMM
     * contiguity assumption in sched_spawn) */
    uint8_t *kstack = (void *)0;
    uint32_t i;
    for (i = 0; i < STACK_PAGES; i++) {
        uint64_t p = pmm_alloc_page();
        if (!p) {
            printk("[PROC] FAIL: OOM allocating kernel stack\n");
            for (;;) {}
        }
        if (i == 0)
            kstack = (uint8_t *)(uintptr_t)p;
    }

    /* Create per-process page tables (kernel high entries shared) */
    proc->pml4_phys = vmm_create_user_pml4();

    /* Load ELF into the user address space */
    uint64_t entry_rip = elf_load(proc->pml4_phys, elf_data, elf_len);
    if (!entry_rip) {
        printk("[PROC] FAIL: ELF parse error\n");
        for (;;) {}
    }

    /* Allocate and map user stack page */
    uint64_t user_stack_phys = pmm_alloc_page();
    if (!user_stack_phys) {
        printk("[PROC] FAIL: OOM allocating user stack\n");
        for (;;) {}
    }
    vmm_map_user_page(proc->pml4_phys, USER_STACK_PAGE, user_stack_phys,
                      VMM_FLAG_USER | VMM_FLAG_WRITABLE);

    /*
     * Build initial kernel stack.
     *
     * ctx_switch pops: r15, r14, r13, r12, rbp, rbx, then ret.
     * The ret target is proc_enter_user (a bare iretq label).
     * iretq pops a ring-3 frame: RIP, CS, RFLAGS, RSP, SS.
     *
     * Stack layout from low (RSP) to high:
     *   [r15=0][r14=0][r13=0][r12=0][rbp=0][rbx=0]
     *   [proc_enter_user]
     *   [entry_rip][CS=0x23][RFLAGS=0x202][user_RSP][SS=0x1B]
     *
     * Push order (high-to-low, decrementing sp):
     *   SS first (highest address), r15 last (lowest = task.rsp value).
     */
    uint64_t *sp = (uint64_t *)(kstack + STACK_SIZE);

    *--sp = 0x1BULL;            /* SS  — user data | RPL=3      */
    *--sp = USER_STACK_TOP;     /* RSP — user stack top          */
    *--sp = 0x202ULL;           /* RFLAGS — IF=1, reserved bit 1 */
    *--sp = 0x23ULL;            /* CS  — user code | RPL=3       */
    *--sp = entry_rip;          /* RIP — ELF entry point         */

    *--sp = (uint64_t)(uintptr_t)proc_enter_user; /* ret → iretq */
    *--sp = 0;                  /* rbx */
    *--sp = 0;                  /* rbp */
    *--sp = 0;                  /* r12 */
    *--sp = 0;                  /* r13 */
    *--sp = 0;                  /* r14 */
    *--sp = 0;                  /* r15 ← task.rsp */

    /* Initialize task fields */
    proc->task.rsp              = (uint64_t)(uintptr_t)sp;
    proc->task.stack_base       = kstack;
    proc->task.kernel_stack_top = (uint64_t)(uintptr_t)(kstack + STACK_SIZE);
    proc->task.tid              = 0xFF;   /* fixed user-process TID for Phase 5 */
    proc->task.is_user          = 1;

    sched_add(&proc->task);
}
```

- [ ] **Step 3: Commit**

```bash
git add kernel/proc/proc.h kernel/proc/proc.c
git commit -m "feat: add aegis_process_t and proc_spawn (ring-3 process creation)"
```

---

### Task 12: Makefile Wiring

**Files:**
- Modify: `Makefile`

- [ ] **Step 1: Add Phase 5 source variables and CFLAGS to Makefile**

Add to `CFLAGS` (after `-Ikernel/sched`):
```makefile
    -Ikernel/proc \
    -Ikernel/elf \
    -Ikernel/syscall
```

Add to `ARCH_SRCS` (after `kernel/arch/x86_64/kbd.c`):
```makefile
    kernel/arch/x86_64/gdt.c \
    kernel/arch/x86_64/tss.c \
    kernel/arch/x86_64/arch_syscall.c
```

Add to `ARCH_ASMS` (after `kernel/arch/x86_64/ctx_switch.asm`):
```makefile
    kernel/arch/x86_64/syscall_entry.asm
```

Add new source variables (after `SCHED_SRCS`):
```makefile
PROC_SRCS    = kernel/proc/proc.c
ELF_SRCS     = kernel/elf/elf.c
SYSCALL_SRCS = kernel/syscall/syscall.c
```

Add new object variables (after `SCHED_OBJS`):
```makefile
PROC_OBJS    = $(patsubst kernel/%.c,$(BUILD)/%.o,$(PROC_SRCS))
ELF_OBJS     = $(patsubst kernel/%.c,$(BUILD)/%.o,$(ELF_SRCS))
SYSCALL_OBJS = $(patsubst kernel/%.c,$(BUILD)/%.o,$(SYSCALL_SRCS))
```

- [ ] **Step 2: Add user binary build chain**

Add after `SCHED_OBJS` line (before `ALL_OBJS`):
```makefile
USER_ELF     = user/init/init.elf
USER_BIN_SRC = user/init/init_bin.c
USER_BIN_OBJ = $(BUILD)/proc/init_bin.o
```

Add user binary build rules (after the `$(CAP_LIB)` rule):
```makefile
$(USER_ELF): user/init/main.c user/init/Makefile
	$(MAKE) -C user/init

$(USER_BIN_SRC): $(USER_ELF)
	cd user/init && xxd -i init.elf > init_bin.c

$(USER_BIN_OBJ): $(USER_BIN_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
```

- [ ] **Step 3: Update ALL_OBJS**

Replace:
```makefile
ALL_OBJS = $(BOOT_OBJ) $(ARCH_OBJS) $(ARCH_ASM_OBJS) $(CORE_OBJS) $(MM_OBJS) $(SCHED_OBJS)
```
With:
```makefile
ALL_OBJS = $(BOOT_OBJ) $(ARCH_OBJS) $(ARCH_ASM_OBJS) $(CORE_OBJS) $(MM_OBJS) \
           $(SCHED_OBJS) $(PROC_OBJS) $(ELF_OBJS) $(SYSCALL_OBJS) $(USER_BIN_OBJ)
```

- [ ] **Step 4: Add `clean` target to remove user build artifacts**

Update `clean`:
```makefile
clean:
	rm -rf $(BUILD)
	$(CARGO) clean --manifest-path kernel/cap/Cargo.toml
	$(MAKE) -C user/init clean
```

- [ ] **Step 5: Verify Makefile builds (link errors expected — main.c not wired yet)**

```bash
make 2>&1 | grep -E "error:|undefined" | head -20
```
Expected: linker errors for `arch_gdt_init`, `proc_spawn` (not yet in main.c) — compile errors would indicate a worse problem.

- [ ] **Step 6: Commit**

```bash
git add Makefile
git commit -m "build: wire Phase 5 sources, user binary build chain into Makefile"
```

---

### Task 13: Wire Phase 5 into main.c

**Files:**
- Modify: `kernel/core/main.c`

- [ ] **Step 1: Add new includes to main.c**

After the existing includes, add:
```c
#include "gdt.h"
#include "proc.h"
```

- [ ] **Step 2: Update `kernel_main` init sequence**

Replace:
```c
    kbd_init();             /* PS/2 keyboard — [KBD] OK                      */
    sched_init();           /* init run queue (no tasks yet)                 */
```
With:
```c
    kbd_init();             /* PS/2 keyboard — [KBD] OK                      */
    arch_gdt_init();        /* ring 3 GDT descriptors — [GDT] OK             */
    arch_tss_init();        /* TSS iomap_base — [TSS] OK                     */
    arch_syscall_init();    /* SYSCALL/SYSRET MSRs — [SYSCALL] OK            */
    sched_init();           /* init run queue (no tasks yet)                 */
```

Add `proc_spawn` call after `sched_spawn(task_heartbeat)`:
```c
    sched_spawn(task_kbd);
    sched_spawn(task_heartbeat);
    proc_spawn(init_elf, init_elf_len);
    sched_start();          /* prints [SCHED] OK, switches into first task   */
```

Note: `init_elf` and `init_elf_len` are declared as `extern` in `proc.c` and resolved
from `init_bin.o` at link time. `proc_spawn` in `proc.h` is declared with these
as parameters, so main.c passes them through.

Wait — looking at `proc.h`, `proc_spawn` takes `(const uint8_t *elf_data, size_t elf_len)`.
The `init_elf`/`init_elf_len` symbols are in `proc.c`'s translation unit (via `extern`).
`main.c` cannot directly reference them. Fix: either add a wrapper `proc_spawn_init(void)`
or declare the externs in main.c too. Cleaner: add `proc_spawn_init` to proc.h.

Update `kernel/proc/proc.h` — add after `proc_spawn`:
```c
/* Convenience wrapper: spawn the embedded init binary (init_elf / init_elf_len). */
void proc_spawn_init(void);
```

Update `kernel/proc/proc.c` — add at the end:
```c
void
proc_spawn_init(void)
{
    proc_spawn(init_elf, (size_t)init_elf_len);
}
```

Then in `main.c`:
```c
    proc_spawn_init();
```

- [ ] **Step 3: Commit**

```bash
git add kernel/core/main.c kernel/proc/proc.h kernel/proc/proc.c
git commit -m "feat: wire GDT/TSS/SYSCALL init and proc_spawn_init into kernel_main"
```

---

### Task 14: GREEN — make test Passes

- [ ] **Step 1: Build the kernel**

```bash
make 2>&1 | tail -20
```
Expected: clean build, no errors or warnings.

Common errors to fix:
- Missing `#include "vmm.h"` in sched.c → add it
- `proc.h` not found → check `-Ikernel/proc` in CFLAGS
- `init_bin.c` not generated → check `$(USER_BIN_SRC)` rule fires before `$(USER_BIN_OBJ)`
- `arch_set_kernel_stack` undefined → check tss.c is in ARCH_SRCS

- [ ] **Step 2: Run the full test suite**

```bash
make test
echo "Exit code: $?"
```
Expected output (exact):
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
[GDT] OK: ring 3 descriptors installed
[TSS] OK: RSP0 initialized
[SYSCALL] OK: SYSCALL/SYSRET enabled
[SCHED] OK: scheduler started, 3 tasks
[USER] hello from ring 3
[USER] hello from ring 3
[USER] hello from ring 3
[USER] done
[AEGIS] System halted.
```
Expected: `Exit code: 0`

- [ ] **Step 3: If QEMU shows a triple fault / reboot loop**

Common causes:
1. GDT order wrong → SYSRET loads data descriptor into CS → #GP → double-fault → triple-fault
   Fix: verify GDT index 3 = user data (0x18), index 4 = user code (0x20)
2. USER bit missing on intermediate PTE → ring-3 #PF
   Fix: verify `ensure_table_user` sets `VMM_FLAG_USER` on PML4e, PDPTe, PDe
3. TSS not loaded with `ltr` → ring-3 interrupt has nowhere to switch RSP → triple-fault
   Fix: verify `arch_gdt_init` calls `ltr` after `lgdt`
4. `proc_enter_user` has C prologue → iretq pops wrong frame
   Fix: verify it is a bare NASM label with just `iretq` (no `push rbp` etc.)
5. `g_kernel_rsp = 0` when first SYSCALL fires → RSP set to 0 → fault
   Fix: verify `arch_set_kernel_stack` is called in `sched_start` before `ctx_switch`

- [ ] **Step 4: If [USER] lines appear in wrong order or interleaved with [AEGIS]**

This should not happen because:
- Each `sys_write` call runs with IF=0 (SFMASK bit 9 cleared on SYSCALL entry)
- init completes all 4 writes before `sys_exit` removes it from the run queue
- task_heartbeat only prints [AEGIS] after 500 ticks (5 seconds at 100 Hz)

If [USER] lines ARE missing, check that `vmm_switch_to(init_pml4)` is called
before `ctx_switch` when switching to the init process.

- [ ] **Step 5: Commit GREEN state**

```bash
git add -A
git commit -m "feat: Phase 5 GREEN — ring-3 user process runs, make test passes"
```

---

### Task 15: Update CLAUDE.md Build Status

**Files:**
- Modify: `.claude/CLAUDE.md`

- [ ] **Step 1: Update Build Status table**

Replace the Phase 4/5 rows:
```markdown
| Scheduler (single-core) | ✅ Done | Round-robin preemptive; sched_add, sched_exit added |
| Syscall dispatch (write, exit) | ✅ Done | Linux-compatible numbers; IF=0 atomic dispatch |
| User space (ring 3) | ✅ Done | ELF64 static loader; proc_spawn; SYSCALL/SYSRET |
| ELF loader (static) | ✅ Done | PT_LOAD segments; PMM contiguity assumption |
| Capability system (Rust) | ⬜ Phase 6 | CLAUDE.md: wait for syscall layer — now solid |
| Mapped-window allocator | ⬜ Phase 6 | Required before identity map teardown |
| VFS | ⬜ Phase 7 | |
| musl port + shell | ⬜ Phase 7+ | |
```

Update the last-updated line:
```markdown
*Last updated: 2026-03-20 — Phase 5 complete, make test GREEN. Ring-3 user process running; SYSCALL/SYSRET enabled.*
```

Add a Phase 5 forward-looking constraints section:

```markdown
### Phase 5 deferred items (must address in Phase 6)

**Identity map / PMM contiguity assumption**: `proc_spawn` and `elf_load` both
use the same PMM contiguity assumption as `sched_spawn`. All Phase 5 allocations
stay within the 4MB identity window. Phase 6 must introduce a mapped-window
allocator before adding any allocation that could exceed 4MB.

**Process memory cleanup**: Page tables, kernel stack, user stack, and ELF segment
pages are leaked on `sched_exit`. Phase 6 cleanup must use `dying->stack_base` (PMM
allocation start) and `dying->rsp` (current stack pointer) — not assume that
`stack_base + STACK_SIZE` is the current RSP. See `sched_exit` comment.

**Pointer validation in sys_write**: A user process can pass a kernel-space address
as the `write` buffer. Phase 6 must add bounds checking and enable SMAP.

**Single-core g_kernel_rsp / g_user_rsp**: These globals assume single-CPU operation.
Phase 6+ must make them per-CPU if SMP is added.
```

- [ ] **Step 2: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs: update CLAUDE.md — Phase 5 complete, Phase 6 deferred items"
```

---

## Quick Reference

### Selector Values (for iretq frames and SYSCALL)
| Use | Value | Meaning |
|-----|-------|---------|
| Ring-3 CS | `0x23` | GDT[4] (user code) \| RPL=3 |
| Ring-3 SS | `0x1B` | GDT[3] (user data) \| RPL=3 |
| Kernel CS | `0x08` | GDT[1] (kernel code) DPL=0 |
| TSS | `0x28` | GDT[5+6] system descriptor |

### STAR MSR Arithmetic
```
STAR[47:32] = 0x0008  →  SYSCALL: CS=0x08, SS=0x10
STAR[63:48] = 0x0010  →  SYSRET:  SS=(0x10+8)|3=0x1B, CS=(0x10+16)|3=0x23
```

### User Address Space
```
0x400000                ELF text (PT_LOAD mapped here)
0x7FFFFFE000            user stack page (4KB)
0x7FFFFFFF000           user stack top  (RSP initial value, 16-byte aligned)
```
