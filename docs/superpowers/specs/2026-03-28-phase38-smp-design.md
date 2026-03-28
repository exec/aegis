# Phase 38: SMP (Symmetric Multiprocessing)

## Goal

Add full multi-core support to the Aegis kernel. Boot all available CPUs via INIT-SIPI-SIPI, replace the 8259A PIC with LAPIC + I/O APIC, add per-CPU scheduling via GS.base, protect all shared state with spinlocks, and implement TLB shootdown for cross-CPU page table coherence.

## Scope

This is a large cross-cutting change. Decomposed into four sequential sub-phases, each producing a testable kernel:

1. **38a: LAPIC + I/O APIC + Spinlocks** — replace PIC with APIC interrupt routing, add spinlock infrastructure, protect critical subsystems
2. **38b: Per-CPU Data + SWAPGS** — GS.base per-CPU struct, move `s_current` to per-CPU, SWAPGS in syscall/ISR entry
3. **38c: AP Startup + LAPIC Timer** — real-mode trampoline, INIT-SIPI-SIPI, per-CPU scheduling, LAPIC timer replaces PIT for preemption
4. **38d: TLB Shootdown** — IPI-based cross-CPU TLB invalidation for munmap/mprotect

## Non-Goals

- NUMA awareness (single memory domain)
- Per-CPU page allocator (global PMM with spinlock is sufficient)
- Work stealing between CPU run queues (simple round-robin per CPU)
- CPU hotplug (all CPUs started at boot, never stopped)
- x2APIC mode (xAPIC MMIO mode is sufficient for <256 CPUs)

---

## Sub-Phase 38a: LAPIC + I/O APIC + Spinlocks

### LAPIC Driver

New file: `kernel/arch/x86_64/lapic.c` + `lapic.h`

**Detection:** Read `IA32_APIC_BASE` MSR (0x1B). Physical base is bits [35:12] (typically 0xFEE00000). Bit 11 = global enable. Bit 8 = BSP flag.

**Initialization:**
1. Map LAPIC MMIO page (4KB at 0xFEE00000) into kernel VA via `kva_alloc_pages(1)` + uncached mapping
2. Write Spurious Interrupt Vector Register (offset 0x0F0): set vector 0xFF, set bit 8 (APIC Enable)
3. Clear Task Priority Register (offset 0x080) to 0 — accept all interrupt priorities
4. Read LAPIC ID (offset 0x020, bits [31:24]) — this is the BSP's APIC ID

**API:**
```c
void     lapic_init(void);              /* BSP LAPIC setup */
void     lapic_init_ap(void);           /* AP LAPIC setup (same as BSP minus detection) */
uint8_t  lapic_id(void);               /* current CPU's LAPIC ID */
void     lapic_eoi(void);              /* end of interrupt */
void     lapic_send_ipi(uint8_t dest, uint8_t vector);
void     lapic_send_ipi_all_excl_self(uint8_t vector);
```

### I/O APIC Driver

New file: `kernel/arch/x86_64/ioapic.c` + `ioapic.h`

**Detection:** Parse MADT entry type 1 (I/O APIC). Get base address and GSI (Global System Interrupt) base. Map I/O APIC MMIO (typically 0xFEC00000) into kernel VA.

**Access:** Indirect register access via index (offset 0x00) and data (offset 0x10) registers.

**IRQ routing:** Each of the 24 redirection table entries maps an IRQ pin to a vector + destination APIC ID. Entry format is 64 bits (two 32-bit registers).

Standard Aegis IRQ routing:

| IRQ | Device | I/O APIC Pin | Vector | Notes |
|-----|--------|-------------|--------|-------|
| 0 | PIT | 2 (via MADT override) | 0x20 | Pin 0 → pin 2 redirect is standard |
| 1 | PS/2 keyboard | 1 | 0x21 | |
| 9 | ACPI SCI | 9 | 0x29 | Level-triggered, active-low |
| 12 | PS/2 mouse | 12 | 0x2C | |

All routes initially target BSP (LAPIC ID from detection). Route to BSP only in 38a; load-balanced routing deferred.

**MADT parsing extension:** The existing `acpi.c` parses MADT for MCFG info but does NOT extract LAPIC entries or I/O APIC entries. Add parsing for:
- Type 0: Processor Local APIC (APIC ID + flags for each CPU)
- Type 1: I/O APIC (address + GSI base)
- Type 2: Interrupt Source Override (ISA IRQ remapping, e.g., IRQ0 → pin 2)

Store in a global array: `smp_cpu_t smp_cpus[MAX_CPUS]` with APIC IDs.

### PIC → APIC Transition

At the end of LAPIC + I/O APIC init:
1. Remap 8259A PIC to vectors 0xF0-0xFF (out of the way)
2. Mask all PIC IRQs (0xFF to both mask ports)
3. Route all active IRQs through I/O APIC instead

The existing `pic_send_eoi()` calls in `idt.c` change to `lapic_eoi()`. The `pic_unmask()` calls in drivers change to `ioapic_route_irq()`.

### Spinlock Infrastructure

New file: `kernel/core/spinlock.h`

Ticket lock implementation:

```c
typedef struct {
    volatile uint16_t owner;
    volatile uint16_t next;
} spinlock_t;

#define SPINLOCK_INIT { 0, 0 }

static inline void spin_lock(spinlock_t *lock);
static inline void spin_unlock(spinlock_t *lock);

/* IRQ-safe variants: disable interrupts before acquiring.
 * Uses arch_irq_save()/arch_irq_restore() from arch.h to keep
 * spinlock.h architecture-agnostic (kernel/core/ must not contain
 * x86 instructions per CLAUDE.md rules). */
typedef unsigned long irqflags_t;
static inline irqflags_t spin_lock_irqsave(spinlock_t *lock);
static inline void spin_unlock_irqrestore(spinlock_t *lock, irqflags_t flags);
```

The `pause` instruction is used in the spin loop to reduce power and improve SMT performance.

### Critical Sections to Protect

Add spinlocks to these subsystems in 38a (even before multi-core):

#### Core Kernel Locks

| Subsystem | Lock | Scope |
|-----------|------|-------|
| Scheduler | `sched_lock` | Global — protects run queue + task state transitions |
| PMM | `pmm_lock` | Global — protects bitmap for alloc/free |
| KVA | `kva_lock` | Global — protects bump allocator |
| VMM window | `vmm_window_lock` | Global — protects s_window_pte page table walks |
| PID | `pid_lock` | Global — protects s_next_pid (or use atomic) |
| RNG | `rng_lock` | Global — protects ChaCha20 state + output buffer |
| Futex | `futex_lock` | Global — protects futex waiter pool |

#### Per-Instance Locks

| Subsystem | Lock | Scope |
|-----------|------|-------|
| fd_table | `fdt->lock` | Per-fd-table — protects fds[] array |
| Pipe | `pipe->lock` | Per-pipe — protects ring buffer + waiters |
| PTY | `pty->lock` | Per-PTY-pair — protects master/slave buffers |
| TTY | `tty->lock` | Per-TTY — protects line discipline state |

#### Network Stack Locks

| Subsystem | Lock | Scope |
|-----------|------|-------|
| Socket table | `sock_lock` | Global — protects s_socks[] array |
| TCP | `tcp_lock` | Global — protects s_tcp[] connection table + shared TX buffer |
| UDP | `udp_lock` | Global — protects s_udp[] binding table + shared TX buffer |
| ARP | `arp_lock` | Global — protects ARP table |
| IP | `ip_lock` | Global — protects s_my_ip/netmask/gateway + shared buffers + loopback queue |
| Netdev | `netdev_lock` | Global — protects device registry |
| Epoll | `epoll_lock` | Global — protects epoll instance table |

#### Driver Locks

| Subsystem | Lock | Scope |
|-----------|------|-------|
| NVMe | `nvme_lock` | Global — protects admin+IO queue state, command ID, bounce buffer |
| xHCI | `xhci_lock` | Global — protects command ring, event ring, per-slot HID state |
| Blkdev | `blkdev_lock` | Global — protects block device registry |
| VGA/FB | `console_lock` | Global — protects cursor position + ANSI state |

#### Filesystem Locks

| Subsystem | Lock | Scope |
|-----------|------|-------|
| ext2 | `ext2_lock` | Global — protects superblock, BGDs, block cache, inode allocation |
| Ramfs | `ramfs->lock` | Per-instance — protects file table |
| Initrd | N/A | Read-only after boot — no lock needed |

#### Read-Only After Init (No Lock Needed)

These are set once during boot and never modified:
- ACPI globals (MCFG, MADT, SCI, PM registers)
- PCIe device list (s_devices in pcie.c) — scan happens once at boot
- Ramdisk base/size
- FB physical address/dimensions
- blkdev entries for ramdisk and NVMe parent (only partitions are dynamic)
- GPT partition table (scanned once; only installer calls gpt_rescan)

In 38a with single core, these locks are never contended but verify correctness (no deadlocks, proper lock ordering).

### Lock Ordering (Deadlock Prevention)

When multiple locks are needed, always acquire in this order:

```
sched_lock > pmm_lock > kva_lock > vmm_window_lock > ext2_lock > nvme_lock >
  fdt->lock > pipe->lock > sock_lock > tcp_lock > udp_lock > arp_lock >
  ip_lock > rng_lock > futex_lock > pid_lock > console_lock
```

Never acquire a lower-priority lock while holding a higher-priority one. `sched_exit` calls `fd_table_unref` which may acquire `fdt->lock`, so scheduler lock must outrank fd_table lock. ext2 operations call NVMe (disk I/O), so ext2_lock outranks nvme_lock.

**Total: ~30 spinlocks across the kernel.** Most are global (single lock per subsystem). Per-instance locks (pipe, PTY, fd_table, ramfs) scale with the number of active objects.

### Testing (38a)

`make test` must still pass — the boot oracle uses `-machine pc` which has no I/O APIC, so the APIC code gracefully skips (MADT has no I/O APIC entry on legacy PC). Spinlocks are acquired/released on single core with zero contention.

For APIC testing: `test_integrated.py` boots on q35 which HAS an I/O APIC. The test must still pass with APIC interrupt routing instead of PIC.

---

## Sub-Phase 38b: Per-CPU Data + SWAPGS

### Per-CPU Structure

```c
#define MAX_CPUS 16

typedef struct percpu {
    struct percpu    *self;          /* offset 0: self-pointer */
    uint8_t           cpu_id;        /* offset 8: logical CPU index (0 = BSP) */
    uint8_t           lapic_id;      /* offset 9: hardware APIC ID */
    uint8_t           _pad[6];       /* padding to align next field */
    aegis_task_t     *current_task;  /* offset 16: currently running task */
    uint64_t          kernel_stack;  /* offset 24: RSP0 for this CPU's TSS */
    uint64_t          ticks;         /* offset 32: per-CPU tick counter */
    aegis_task_t     *idle_task;     /* offset 40: per-CPU idle task */
    /* Run queue head for this CPU (38c) */
    aegis_task_t     *run_head;      /* offset 48: head of per-CPU run queue */
    uint32_t          run_count;     /* offset 56: tasks in this CPU's queue */
} percpu_t;
```

### GS.base Setup

Each CPU's `IA32_KERNEL_GS_BASE` (MSR 0xC0000102) points to its `percpu_t`. The `SWAPGS` instruction swaps `GS.base` with `IA32_KERNEL_GS_BASE`:
- On kernel entry (from ring 3): `SWAPGS` loads the per-CPU struct
- On kernel exit (to ring 3): `SWAPGS` restores user GS.base

**BSP setup:** After allocating `percpu_t` for CPU 0, write to `IA32_KERNEL_GS_BASE`. Add `SWAPGS` to:
1. `syscall_entry` in `syscall_entry.asm` (first instruction)
2. `isr_common_stub` in `isr_common_stub.asm` (when entering from ring 3, check CS on stack)
3. Before `sysret`/`iretq` to ring 3

**Replacing s_current:** The global `s_current` in `sched.c` becomes `percpu->current_task`, accessed via `gs:16`. Provide a `sched_current()` inline that reads from GS.

```c
static inline aegis_task_t *sched_current(void) {
    aegis_task_t *t;
    __asm__ volatile("movq %%gs:16, %0" : "=r"(t));
    return t;
}
```

All callers of `sched_current()` (there are many — proc.c, sys_process.c, sys_io.c, etc.) already use the function. The change is transparent.

### Per-CPU TSS

Each CPU needs its own TSS with its own RSP0. The existing single TSS becomes `tss[MAX_CPUS]`. On AP startup, each CPU loads its own TSS via `ltr`.

### Per-CPU GDT

Each CPU needs its own GDT (because the TSS descriptor points to a per-CPU TSS). Alternatively, all CPUs share a GDT but each has a separate TSS descriptor entry.

Simpler approach: one GDT with MAX_CPUS TSS entries. CPU N uses TSS entry at GDT index `(5 + N)`. Each CPU does `ltr(tss_selector_for_cpu_n)`.

### Per-CPU Kernel/User RSP

**Critical:** `syscall_entry.asm` currently uses global `g_kernel_rsp` and `g_user_rsp` to switch stacks on syscall entry. On SMP, two CPUs taking syscalls simultaneously clobber each other's saved RSP.

Fix: replace `g_kernel_rsp`/`g_user_rsp` with per-CPU fields accessed via `gs:offset`:
- `percpu->kernel_stack` (offset 24) replaces `g_kernel_rsp`
- User RSP saved to `percpu` scratch area (new field at offset 64) replaces `g_user_rsp`

`syscall_entry.asm` changes from:
```nasm
mov [rel g_user_rsp], rsp
mov rsp, [rel g_kernel_rsp]
```
to:
```nasm
swapgs                         ; GS.base now points to percpu
mov [gs:64], rsp               ; save user RSP to percpu scratch
mov rsp, [gs:24]               ; load kernel stack from percpu
```

### IDT Expansion

The current IDT has 48 entries (vectors 0x00-0x2F). SMP requires additional vectors:
- 0xFF: LAPIC spurious interrupt
- 0xFE: TLB shootdown IPI
- 0xFD: reschedule IPI (cross-CPU wakeup)

Expand `s_idt` to 256 entries. Only vectors with handlers need gate installation; unused vectors remain zeroed (generates #GP on delivery, which is correct for unexpected interrupts).

### SWAPGS Placement in ISR Path

**Critical sequencing:** SWAPGS must execute BEFORE any register saves and BEFORE any GS-relative access. The check for ring-3 transition (was this interrupt from user mode?) uses the CS value pushed by the CPU on the interrupt stack frame.

In `isr_common_stub.asm`, the correct order is:
1. CPU pushes SS, RSP, RFLAGS, CS, RIP (and error code if applicable)
2. ISR stub pushes error code placeholder (if none) + vector number
3. **Check CS on stack: if CS == 0x23, execute SWAPGS**
4. Push all GPRs
5. Save CR3, switch to master PML4
6. Call `isr_dispatch`
7. Restore CR3
8. Pop all GPRs
9. **If returning to ring 3 (CS == 0x23), execute SWAPGS**
10. `iretq`

The CS value is at a known stack offset after the ISR stub pushes vector + error code. The SWAPGS check reads this offset without disturbing registers (use the stack-relative address).

### Testing (38b)

Still single-core. `make test` must pass. The SWAPGS changes are exercised by every syscall and interrupt. If anything is wrong, the kernel panics immediately.

---

## Sub-Phase 38c: AP Startup + LAPIC Timer

### AP Trampoline

New file: `kernel/arch/x86_64/ap_trampoline.asm`

A small piece of code that lives at physical address 0x8000 (below 1MB, page-aligned). The BSP copies it there before sending SIPIs.

AP startup flow:
1. Start in 16-bit real mode (CS = 0x0800, IP = 0x0000)
2. Load a temporary GDT with 32-bit and 64-bit segments
3. Enable protected mode (CR0.PE)
4. Enable PAE (CR4.PAE)
5. Load the BSP's PML4 into CR3
6. Enable long mode (EFER.LME) and paging (CR0.PG)
7. Far-jump to 64-bit code
8. Load per-CPU kernel stack from a table (indexed by LAPIC ID via CPUID)
9. Jump to C entry point `ap_entry()`

### AP C Entry

New function in `kernel/core/smp.c`:

```c
void ap_entry(void) {
    /* We are on the AP's kernel stack, in long mode, with BSP's PML4 */
    lapic_init_ap();        /* enable this AP's LAPIC */
    percpu_init(cpu_id);    /* set up GS.base for this AP */
    gdt_load();             /* load GDT */
    tss_load(cpu_id);       /* load per-CPU TSS */
    idt_load();             /* shared IDT */
    lapic_timer_init();     /* per-CPU LAPIC timer */

    /* Signal BSP that this AP is online */
    atomic_set(&ap_online[cpu_id], 1);

    /* Enter idle loop — this AP's scheduler runs when tasks are assigned */
    sched_idle_loop();
}
```

### BSP Startup Sequence

In `kernel_main()`, after all subsystem init:

```c
smp_init();  /* parse MADT for CPUs, start each AP */
```

`smp_init()`:
1. Copy trampoline code to physical 0x8000
2. Write BSP's PML4 physical address to trampoline data area
3. Allocate per-CPU stacks (one KVA page each)
4. Write stack pointers to trampoline data area
5. For each AP (from MADT LAPIC entries, excluding BSP):
   a. Send INIT IPI
   b. Wait 10ms
   c. Send SIPI (vector = 0x08 for address 0x8000)
   d. Wait 200μs
   e. Send second SIPI
   f. Poll `ap_online[cpu_id]` until set (timeout after 100ms)
6. Print `[SMP] OK: N CPUs online`

### LAPIC Timer

Replace PIT as the scheduling tick source. Each CPU calibrates its own LAPIC timer using PIT channel 2 as a reference (one-shot 10ms measurement).

Configuration: periodic mode, vector 0x20 (same as PIT was), fire every ~10ms (100 Hz).

The PIT continues to exist for:
- LAPIC timer calibration
- Legacy fallback on systems without LAPIC
- `arch_get_ticks()` still uses a global counter (BSP's LAPIC timer increments it, or all CPUs increment atomically)

### Per-CPU Scheduling

The scheduler changes from a single global circular list to per-CPU run queues:

- Each CPU has `percpu->run_head` — head of its local run queue (circular linked list)
- `sched_spawn()` assigns new tasks to the CPU with the fewest tasks (simple load balancing)
- `sched_tick()` is now per-CPU: advance `percpu->current_task` to next RUNNING task in local queue
- `sched_block()`/`sched_wake()`: if the woken task is on a different CPU's queue, send an IPI to reschedule that CPU
- `sched_exit()`: remove from local queue, deferred cleanup by same CPU

**Migration:** A task is assigned to a CPU at creation and stays there. No work stealing in v1.

### Testing (38c)

This is the first sub-phase where multiple CPUs are actually running. Tests:
- `make test` (boot oracle on `-machine pc`): only BSP runs, LAPIC timer replaces PIT. Must pass.
- `test_integrated.py` (q35): APs boot, print `[SMP] OK: N CPUs online`. All existing tests must still pass.
- New: `test_smp.py` — boot q35 with `-smp 4`, verify `[SMP] OK: 4 CPUs online`, run thread_test (creates threads — should be scheduled across CPUs), verify no panics.

---

## Sub-Phase 38d: TLB Shootdown

### When Shootdown Is Needed

Any operation that modifies user-space page tables while other CPUs might be running the same address space:
- `vmm_unmap_user_page()` — called by munmap, process exit
- `vmm_map_user_page()` with different permissions — called by mprotect
- `vmm_free_user_pml4()` — process exit cleanup

Kernel-space page table changes (KVA) don't need shootdown because all CPUs share the kernel PML4 and kernel mappings don't change at runtime (they're established at boot).

### Protocol

Dedicated IPI vector: `TLB_SHOOTDOWN_VECTOR = 0xFE`

Shared shootdown request structure (global, protected by spinlock):

```c
struct tlb_shootdown {
    spinlock_t   lock;
    volatile uint64_t target_cr3;
    volatile uint64_t va_start;
    volatile uint64_t va_end;
    volatile uint16_t pending;   /* bitmask of CPUs that must respond */
    volatile uint16_t ack;       /* bitmask of CPUs that have responded */
};
```

**Requesting CPU:**
1. Acquire shootdown lock
2. Fill in target CR3, VA range
3. Set pending = bitmask of CPUs running this CR3 (check each CPU's `percpu->current_task->pml4_phys`)
4. Do local `invlpg` for each page in range
5. Send IPI to all-excluding-self (vector 0xFE)
6. Spin on `ack == pending`
7. Release lock

**Receiving CPU (ISR for vector 0xFE):**
1. Check if my current CR3 matches `target_cr3`
2. If yes: `invlpg` for each page in range
3. Set my bit in `ack`
4. `lapic_eoi()`

### Optimization: Lazy Shootdown for User Pages

For user-space unmaps where the process isn't running on other CPUs (common case — single-threaded process): skip the IPI entirely. Only send IPIs when `pending != 0` (other CPUs actually have this CR3 loaded).

For `vmm_free_user_pml4()` (process exit): no shootdown needed because the process is being destroyed and no other CPU will load its CR3 again.

### Testing (38d)

- Thread test with shared memory: parent writes page, child reads, parent unmaps — child must not see stale TLB entry
- Stress test: multiple threads on different CPUs doing mmap/munmap in a loop

---

## File Structure

### New Files

| File | Responsibility |
|------|---------------|
| `kernel/core/spinlock.h` | Ticket spinlock (arch-agnostic API; IRQ save/restore via `arch_irq_save`/`arch_irq_restore` from arch.h) |
| `kernel/arch/x86_64/lapic.c` | Local APIC driver |
| `kernel/arch/x86_64/lapic.h` | LAPIC API |
| `kernel/arch/x86_64/ioapic.c` | I/O APIC driver |
| `kernel/arch/x86_64/ioapic.h` | I/O APIC API |
| `kernel/arch/x86_64/ap_trampoline.asm` | AP real-mode → long-mode trampoline |
| `kernel/arch/x86_64/smp.c` | SMP init, AP entry, CPU enumeration (x86-specific: SIPI, LAPIC, GS.base) |
| `kernel/arch/x86_64/smp.h` | SMP API, percpu_t, MAX_CPUS |
| `kernel/arch/x86_64/tlb.c` | TLB shootdown protocol (x86-specific: invlpg, IPI) |
| `kernel/arch/x86_64/tlb.h` | TLB shootdown API |

### Modified Files

| File | Changes |
|------|---------|
| `kernel/arch/x86_64/acpi.c` | Parse MADT LAPIC + I/O APIC + override entries |
| `kernel/arch/x86_64/idt.c` | Replace `pic_send_eoi` with `lapic_eoi`, add SWAPGS, add shootdown vector |
| `kernel/arch/x86_64/syscall_entry.asm` | Add SWAPGS on entry/exit |
| `kernel/arch/x86_64/isr_common_stub.asm` | Add SWAPGS for ring-3 interrupts |
| `kernel/arch/x86_64/gdt.c` | Per-CPU TSS entries |
| `kernel/arch/x86_64/tss.c` | Per-CPU TSS initialization |
| `kernel/arch/x86_64/pit.c` | Keep as calibration source, remove as primary tick |
| `kernel/sched/sched.c` | Per-CPU run queues, spinlock protection |
| `kernel/sched/sched.h` | percpu_t forward decl, sched_current() via GS |
| `kernel/mm/pmm.c` | Add pmm_lock spinlock |
| `kernel/mm/kva.c` | Add kva_lock spinlock |
| `kernel/mm/vmm.c` | Add vmm_window_lock, TLB shootdown calls |
| `kernel/fs/pipe.c` | Add per-pipe spinlock |
| `kernel/syscall/futex.c` | Add futex_lock spinlock |
| `kernel/proc/proc.c` | Add pid_lock spinlock |
| `kernel/core/main.c` | Call smp_init() after subsystem init |
| `Makefile` | Add new source files |
| `tests/expected/boot.txt` | No change (pc has no SMP) |

---

## Testing Strategy

### Boot Oracle (`make test`, `-machine pc`)

No changes to boot.txt. On `-machine pc`:
- No I/O APIC → LAPIC/IOAPIC init skips gracefully
- No MADT LAPIC entries (or only BSP) → `smp_init()` prints `[SMP] OK: 1 CPU online` (BSP only)
- PIT remains as tick source (LAPIC timer calibration skipped without LAPIC)

Wait — actually `-machine pc` DOES have a LAPIC (it's in the CPU, not the chipset). The I/O APIC is what's missing on legacy PC. So:
- LAPIC init succeeds on BSP
- I/O APIC init skips (no MADT type 1 entry) → fall back to 8259A PIC
- SMP init finds only BSP in MADT → prints `[SMP] OK: 1 CPU online`
- LAPIC timer calibration works, replaces PIT as tick source

Boot oracle needs update. New lines in `tests/expected/boot.txt`:
- `[LAPIC] OK: ...` — after `[KBD]`/`[MOUSE]` lines, before `[GDT]`
- `[IOAPIC] OK: ...` or `[IOAPIC] skipped (no MADT entry)` — after `[LAPIC]`
- `[SMP] OK: 1 CPU online` — after `[SCHED]`, before end

**Note on `-machine pc`:** QEMU's SeaBIOS on `-machine pc` generates MADT with a single LAPIC entry (BSP). LAPIC init will succeed. I/O APIC init will skip (no MADT type 1 entry) and fall back to PIC. Verify empirically.

**`s_current` refactor scope:** `sched.c` accesses `s_current` directly ~40 times (not via `sched_current()`). All must change to `percpu->current_task` via GS-relative access or wrapper. This is the largest single refactor in 38b.

### test_integrated.py (q35, `-smp 1`)

Same as current config. LAPIC + I/O APIC active, single CPU. All 25 tests must pass.

### test_smp.py (q35, `-smp 4`)

New test:
1. Boot with 4 CPUs
2. Verify `[SMP] OK: 4 CPUs online`
3. Run thread_test (already exists — creates threads via clone)
4. Verify threads complete without panic
5. Run `cat /proc/meminfo` — verify memory stats are consistent
6. Verify no kernel panics after 10 seconds of activity

---

## Forward Constraints

1. **No work stealing.** Tasks are assigned to a CPU at creation and stay there. Load imbalance is possible. Work stealing is future work.

2. **No CPU affinity API.** No `sched_setaffinity` syscall. Tasks cannot request specific CPUs.

3. **Global scheduler lock.** The run queue is per-CPU but `sched_wake()` (cross-CPU wakeup) still acquires the global scheduler lock. High contention under heavy threading. Per-CPU locks with cross-CPU IPI wakeup is future work.

4. **MAX_CPUS = 16.** Hardcoded limit. Sufficient for all test hardware. Raising requires increasing percpu array and TSS/GDT entries.

5. **No NUMA.** All memory treated as uniform. No per-node allocation.

6. **LAPIC timer frequency assumed uniform.** All CPUs use the same calibrated frequency. Asymmetric CPUs (big.LITTLE) are not supported.

7. **No CPU offline/hotplug.** All CPUs started at boot, never stopped.

8. **8259A PIC kept as fallback.** If no I/O APIC is found (legacy PC), the kernel continues using the PIC with single-core scheduling. This preserves `make test` compatibility.

9. **Kernel preemption model unchanged.** Kernel code is non-preemptible (interrupts disabled during spinlock holds). Only user-mode code is preempted by timer ticks.
