# Phase 38c: AP Startup + LAPIC Timer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Boot all Application Processors via INIT-SIPI-SIPI, calibrate per-CPU LAPIC timers, and enable multi-core scheduling. The BSP remains the primary executor; APs enter idle loops and receive tasks via `sched_spawn`.

**Architecture:** A real-mode trampoline at physical 0x8000 transitions APs through 16-bit → 32-bit → 64-bit mode using the BSP's PML4. Each AP initializes its LAPIC, GDT/TSS, GS.base (per-CPU data), then enters a scheduler idle loop. The LAPIC timer replaces the PIT as the per-CPU scheduling tick source.

**Tech Stack:** NASM (trampoline), C (SMP init, LAPIC timer), x86-64 MSRs, LAPIC MMIO

---

## File Structure

| File | Responsibility |
|------|---------------|
| `kernel/arch/x86_64/ap_trampoline.asm` | **New.** 16-bit → 32-bit → 64-bit AP bootstrap code |
| `kernel/arch/x86_64/smp.c` | **Modify.** AP startup (INIT-SIPI-SIPI), AP C entry, per-CPU idle task |
| `kernel/arch/x86_64/smp.h` | **Modify.** Add smp_start_aps(), AP online flags |
| `kernel/arch/x86_64/lapic.c` | **Modify.** Add LAPIC timer calibration + periodic mode |
| `kernel/arch/x86_64/lapic.h` | **Modify.** Add lapic_timer_init() |
| `kernel/arch/x86_64/gdt.c` | **Modify.** Per-CPU TSS descriptors (MAX_CPUS TSS entries) |
| `kernel/arch/x86_64/tss.c` | **Modify.** Per-CPU TSS array |
| `kernel/arch/x86_64/idt.c` | **Modify.** Add LAPIC timer vector handler |
| `kernel/arch/x86_64/pit.c` | **Modify.** Keep as calibration source, optionally disable tick after LAPIC timer is active |
| `kernel/sched/sched.c` | **Modify.** Per-CPU idle task, task assignment to CPUs |
| `kernel/core/main.c` | **Modify.** Call smp_start_aps() |
| `Makefile` | **Modify.** Add ap_trampoline.asm |
| `tests/expected/boot.txt` | **Modify.** Add SMP CPU count line |

---

### Task 1: AP Trampoline Assembly

**Files:**
- Create: `kernel/arch/x86_64/ap_trampoline.asm`
- Modify: `Makefile`

The AP trampoline is copied to physical address 0x8000 by the BSP before sending SIPIs. APs start executing here in 16-bit real mode.

- [ ] **Step 1: Create `kernel/arch/x86_64/ap_trampoline.asm`**

The trampoline must:
1. Start in 16-bit real mode at CS:IP = 0x0800:0x0000 (physical 0x8000)
2. Load a temporary GDT with 32-bit and 64-bit segments
3. Enable protected mode (CR0.PE)
4. Far-jump to 32-bit code
5. Enable PAE (CR4.PAE bit 5)
6. Load BSP's PML4 into CR3 (from a data slot filled by BSP)
7. Enable long mode (EFER.LME bit 8) + NXE (bit 11)
8. Enable paging (CR0.PG bit 31) + write protect (CR0.WP bit 16)
9. Far-jump to 64-bit code
10. Set segment registers to kernel data (0x10)
11. Get LAPIC ID via CPUID leaf 1 → EBX[31:24]
12. Look up per-CPU stack from a table (filled by BSP)
13. Jump to the C entry point `ap_entry` in higher-half VA

Data area (filled by BSP before SIPI):
- `ap_pml4_phys` (4 bytes): physical address of kernel PML4
- `ap_stack_table` (MAX_CPUS × 8 bytes): per-CPU kernel stack tops
- `ap_entry_addr` (8 bytes): VA of the C function `ap_entry`

The trampoline must fit in one 4KB page. Use NASM `[BITS 16]`, `[BITS 32]`, `[BITS 64]` directives. All code must be position-independent relative to 0x8000 (use `TRAMPOLINE_PHYS equ 0x8000` and compute addresses as `label - ap_trampoline_start + TRAMPOLINE_PHYS`).

Key GDT entries for the trampoline:
- null (0x00)
- 32-bit code (0x08): `0x00CF9A000000FFFF`
- 32-bit data (0x10): `0x00CF92000000FFFF`
- 64-bit code (0x18): `0x00AF9A000000FFFF`
- 64-bit data (0x20): `0x00AF92000000FFFF`

After entering 64-bit mode, the AP uses the same higher-half kernel mapping as the BSP (it loaded the BSP's PML4 via CR3). It can then jump to a higher-half VA for the C entry.

The trampoline is assembled into a special section `.text.ap_trampoline` and the BSP copies it to 0x8000 using a `memcpy` through the identity map (0-1GB is identity-mapped during boot).

- [ ] **Step 2: Add to Makefile**

Add `kernel/arch/x86_64/ap_trampoline.asm` as an NASM source. It needs to be assembled with `-f elf64` like other .asm files. The trampoline symbols are referenced by smp.c to copy the code to 0x8000.

Add to ARCH_ASM_SRCS (or equivalent):
```
kernel/arch/x86_64/ap_trampoline.asm
```

- [ ] **Step 3: Verify build**

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/ap_trampoline.asm Makefile
git commit -m "feat(phase38c): AP trampoline — 16-bit real to 64-bit long mode"
```

---

### Task 2: SMP AP Startup — INIT-SIPI-SIPI Protocol

**Files:**
- Modify: `kernel/arch/x86_64/smp.c`
- Modify: `kernel/arch/x86_64/smp.h`

- [ ] **Step 1: Add AP startup infrastructure to smp.h**

```c
/* AP online flags — BSP polls these after sending SIPI */
extern volatile uint8_t g_ap_online[MAX_CPUS];

/* Start all Application Processors discovered in MADT.
 * Called from kernel_main after all subsystems are initialized. */
void smp_start_aps(void);

/* C entry point for APs. Called from ap_trampoline after long mode transition. */
void ap_entry(void);
```

- [ ] **Step 2: Implement AP startup in smp.c**

Add to smp.c:

1. `smp_start_aps()`:
   - Copy trampoline code to physical 0x8000 (using identity-mapped pointer `(void *)0x8000` — still mapped in BSP's PML4)
   - Fill trampoline data area: `ap_pml4_phys` = `vmm_get_master_pml4()`, `ap_entry_addr` = VA of `ap_entry`, per-CPU stack pointers (allocate via `kva_alloc_pages(STACK_PAGES)` per AP)
   - For each CPU in `g_smp_cpus[]` (excluding BSP by APIC ID):
     a. Init percpu_t for this CPU
     b. Send INIT IPI via `lapic_send_ipi(apic_id, 0)` with delivery=INIT (ICR bits: `0x00004500`)
     c. Wait 10ms (busy-loop using PIT ticks: `while (arch_get_ticks() < start + 1) {}` — 1 tick at 100Hz = 10ms)
     d. Send SIPI via ICR: `0x00004600 | (0x8000 >> 12)` = `0x00004608`
     e. Wait 200μs (busy-loop ~20000 iterations)
     f. Send second SIPI (same)
     g. Poll `g_ap_online[cpu_id]` for up to 100ms
   - Print `[SMP] OK: N CPUs online\n`

2. `ap_entry()`:
   - Called by trampoline in 64-bit mode with per-CPU stack loaded
   - Determine CPU ID from LAPIC ID → index into g_smp_cpus
   - Init percpu GS.base: `arch_set_gs_base(&g_percpu[cpu_id])`, `arch_write_kernel_gs_base(&g_percpu[cpu_id])`
   - Call `lapic_init_ap()` (configure SVR + TPR)
   - Load kernel GDT (already in memory), load per-CPU TSS
   - Load IDT (shared, already in memory)
   - Set `g_ap_online[cpu_id] = 1` (signal BSP)
   - Enter idle: `for (;;) { __asm__ volatile("sti; hlt; cli"); }`

Note: Per-CPU scheduling and LAPIC timer come in Tasks 3+4. For now APs just idle.

- [ ] **Step 3: Wire into main.c**

After all subsystem init but before `sched_start()`, add:
```c
    smp_start_aps();  /* start APs — [SMP] OK: N CPUs online */
```

- [ ] **Step 4: Update boot.txt**

On `-machine pc` with `-smp 1` (default), only BSP runs. The `[SMP]` line changes from:
```
[SMP] OK: per-CPU data initialized
```
to printing once at BSP init and once after AP startup. Simplify: remove the BSP-init printk and only print after smp_start_aps:
```
[SMP] OK: 1 CPU online
```

- [ ] **Step 5: Verify build**

- [ ] **Step 6: Commit**

---

### Task 3: LAPIC Timer — Calibration + Periodic Mode

**Files:**
- Modify: `kernel/arch/x86_64/lapic.c`
- Modify: `kernel/arch/x86_64/lapic.h`
- Modify: `kernel/arch/x86_64/idt.c`
- Modify: `kernel/arch/x86_64/pit.c`

- [ ] **Step 1: Add LAPIC timer API to lapic.h**

```c
/* Calibrate LAPIC timer using PIT channel 2. Must be called on each CPU.
 * Configures periodic mode at ~100Hz. */
void lapic_timer_init(void);

/* LAPIC timer ISR handler — called from idt.c on the timer vector. */
void lapic_timer_handler(void);
```

- [ ] **Step 2: Implement LAPIC timer in lapic.c**

Add LAPIC timer register offsets:
```c
#define LAPIC_TIMER_LVT  0x320
#define LAPIC_TIMER_ICR  0x380   /* Initial Count */
#define LAPIC_TIMER_CCR  0x390   /* Current Count */
#define LAPIC_TIMER_DCR  0x3E0   /* Divide Configuration */
```

LVT Timer register bits:
- Bits [7:0]: vector number (use 0x20 — same as PIT was)
- Bit 16: mask (1=masked)
- Bit 17: mode (0=one-shot, 1=periodic)

Calibration using PIT channel 2:
```c
void lapic_timer_init(void) {
    if (!s_active) return;

    /* Set divide to 16 */
    lapic_write(LAPIC_TIMER_DCR, 0x03);

    /* Mask the timer during calibration */
    lapic_write(LAPIC_TIMER_LVT, 0x00010020); /* masked, vector 0x20 */

    /* Program PIT channel 2 for 10ms one-shot */
    outb(0x61, (inb(0x61) & 0xFD) | 0x01);  /* gate on */
    outb(0x43, 0xB0);  /* channel 2, lo/hi, mode 0, binary */
    uint16_t pit_count = 11932;  /* ~10ms at 1193182 Hz */
    outb(0x42, pit_count & 0xFF);
    outb(0x42, pit_count >> 8);

    /* Start LAPIC countdown from max */
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);

    /* Wait for PIT channel 2 to expire (bit 5 of port 0x61 goes high) */
    while (!(inb(0x61) & 0x20))
        arch_pause();

    /* Read how many LAPIC ticks elapsed in 10ms */
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);

    /* Configure periodic mode: fire every ~10ms (100Hz) */
    lapic_write(LAPIC_TIMER_LVT, (1u << 17) | 0x20); /* periodic, vector 0x20, unmasked */
    lapic_write(LAPIC_TIMER_ICR, elapsed);
}
```

Timer handler:
```c
void lapic_timer_handler(void) {
    /* Increment per-CPU tick counter */
    percpu_t *p = percpu_self();
    if (p) p->ticks++;

    /* Call scheduler tick (same as PIT handler did) */
    sched_tick();
}
```

- [ ] **Step 3: Add LAPIC timer vector to idt.c**

In `isr_dispatch`, the LAPIC timer fires on vector 0x20 (same as PIT). When LAPIC is active, vector 0x20 comes from the LAPIC timer, not the PIT. The handler dispatch already calls `pit_handler()` for vector 0x20. Change this:

```c
if (s->vector == 0x20) {
    if (lapic_active())
        lapic_timer_handler();
    else
        pit_handler();
}
```

- [ ] **Step 4: Call lapic_timer_init on BSP**

In `smp_start_aps()`, after all APs are started, call `lapic_timer_init()` on the BSP. Each AP also calls `lapic_timer_init()` in `ap_entry()`.

- [ ] **Step 5: Optionally mask PIT when LAPIC timer is active**

If LAPIC timer is calibrated, mask PIT IRQ0 via I/O APIC (or just let it fire — the handler checks `lapic_active()` and dispatches accordingly). Simpler: keep PIT running but the handler does nothing when LAPIC is active (the `if (lapic_active())` check handles this).

Actually, better approach: when LAPIC timer is active, the PIT still fires on vector 0x20 (since I/O APIC routes it there). We need to either mask the PIT via I/O APIC or handle both. Simplest: mask PIT pin in I/O APIC after LAPIC timer is calibrated.

```c
if (ioapic_active())
    ioapic_mask(pit_pin);  /* PIT no longer needed */
```

But the PIT pin might be 0 or 2 (ISO remap). Store the PIT pin during ioapic_init. Actually, the PIT is still needed for things like `arch_get_ticks()`. Keep PIT running on the BSP for now; the LAPIC timer handler is what drives scheduling.

For simplicity in v1: on `-machine pc` (no I/O APIC), PIT continues as-is. On q35 (I/O APIC + LAPIC), the LAPIC timer fires on a DIFFERENT vector (use 0x30 instead of 0x20 to avoid conflict with PIT). Then both can coexist.

**Revised approach:** Use vector 0x30 for LAPIC timer. Add handler in idt.c:
```c
else if (s->vector == 0x30) { lapic_timer_handler(); }
```

Update LAPIC timer LVT to use vector 0x30.

- [ ] **Step 6: Verify build**

- [ ] **Step 7: Commit**

---

### Task 4: Per-CPU GDT/TSS

**Files:**
- Modify: `kernel/arch/x86_64/gdt.c`
- Modify: `kernel/arch/x86_64/tss.c`

Each AP needs its own TSS (different RSP0 per CPU). Options:
- Per-CPU GDT with one TSS entry each → each CPU loads its own GDT
- Shared GDT with MAX_CPUS TSS entries → each CPU does `ltr` with a different selector

Shared GDT approach is simpler. Add MAX_CPUS TSS entries:

- [ ] **Step 1: Expand GDT for per-CPU TSS**

Current GDT layout:
- [0] null, [1] kernel code, [2] kernel data, [3] user data, [4] user code, [5-6] TSS (BSP)

New layout:
- [0] null, [1] KC, [2] KD, [3] UD, [4] UC, [5-6] TSS0, [7-8] TSS1, ... [5+2N, 6+2N] TSSN

Add `arch_gdt_set_ap_tss(uint8_t cpu_id, uint64_t tss_base)` function.

- [ ] **Step 2: Per-CPU TSS array**

In `tss.c`, change from single `s_tss` to `s_tss[MAX_CPUS]`. Each AP initializes its own TSS in `ap_entry()`.

Add `arch_tss_init_ap(uint8_t cpu_id)` — sets up TSS for the given CPU with its own IST1 stack and iomap_base.

- [ ] **Step 3: AP loads its TSS**

In `ap_entry()`, each AP:
1. Calls `arch_gdt_set_ap_tss(cpu_id, &s_tss[cpu_id])`
2. Loads GDT with `lgdt` (shared GDTR)
3. Loads TSS with `ltr(0x28 + cpu_id * 16)` (selector for its TSS entry)

- [ ] **Step 4: Commit**

---

### Task 5: Integration + Test

**Files:**
- Modify: `tests/expected/boot.txt`
- Create: `tests/test_smp.py` (optional)

- [ ] **Step 1: Update boot.txt**

Replace `[SMP] OK: per-CPU data initialized` with `[SMP] OK: 1 CPU online` (since `-machine pc` + `-smp 1` has only BSP).

- [ ] **Step 2: Build and test on DO box**

```bash
ssh root@138.197.76.116
cd /root/aegis && git pull && make clean && make test
```

Expected: boot oracle passes with `[SMP] OK: 1 CPU online`.

Then test with SMP:
```bash
qemu-system-x86_64 -machine q35 -cpu Broadwell -smp 4 -m 2G \
  -cdrom build/aegis.iso -boot order=d \
  -display none -vga std -nodefaults -serial stdio -no-reboot \
  -drive file=build/disk.img,if=none,id=nvme0,format=raw \
  -device nvme,drive=nvme0,serial=aegis0 \
  -device isa-debug-exit,iobase=0xf4,iosize=0x04
```

Expected: `[SMP] OK: 4 CPUs online` in serial output, no panics.

- [ ] **Step 3: Commit fixes**

---

## Self-Review

**1. Spec coverage:**
- [x] AP trampoline (real → protected → long mode) → Task 1
- [x] INIT-SIPI-SIPI protocol → Task 2
- [x] Per-CPU GS.base for APs → Task 2
- [x] LAPIC timer calibration (PIT ch2) → Task 3
- [x] LAPIC timer periodic mode → Task 3
- [x] Per-CPU TSS → Task 4
- [x] AP idle loop → Task 2
- [x] smp_start_aps in main.c → Task 2
- [x] Boot oracle update → Task 5

**2. Placeholder scan:** None.

**3. Type consistency:** `percpu_t`, `g_percpu[]`, `g_ap_online[]`, `lapic_timer_init()`, `lapic_timer_handler()`, `ap_entry()` used consistently.

**Research findings incorporated:**
- INIT-SIPI timing: 10ms + 200μs (Intel SDM)
- PIT channel 2 calibration: 11932 count for ~10ms
- xAPIC MMIO writes are serializing: no MFENCE before SIPI needed
- LAPIC timer vector: use 0x30 to avoid PIT conflict
- NMI + SWAPGS: not a concern (NMI → panic, not handled)
