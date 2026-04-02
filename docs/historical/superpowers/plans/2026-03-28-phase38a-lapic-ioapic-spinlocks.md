# Phase 38a: LAPIC + I/O APIC + Spinlocks — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the 8259A PIC with LAPIC + I/O APIC interrupt routing on q35, add spinlock infrastructure, and protect all critical kernel subsystems with spinlocks — while maintaining single-core boot oracle compatibility on `-machine pc`.

**Architecture:** Parse MADT for LAPIC/I/O APIC entries. Map LAPIC MMIO, enable it, configure spurious vector. Map I/O APIC MMIO, route IRQs. Disable 8259A PIC. Expand IDT to 256 entries. Add ticket spinlocks to PMM, KVA, VMM, scheduler, pipes, futex, fd_table, networking, NVMe, ext2, RNG, and all other shared state. Graceful fallback: if no I/O APIC in MADT, keep using PIC (preserves `-machine pc` boot oracle).

**Tech Stack:** C (kernel), NASM (IDT stubs), x86 LAPIC/IOAPIC MMIO registers

---

## File Structure

| File | Responsibility |
|------|---------------|
| `kernel/core/spinlock.h` | **New.** Ticket spinlock + IRQ-safe variants via `arch_irq_save`/`arch_irq_restore` |
| `kernel/arch/x86_64/lapic.c` | **New.** LAPIC init, enable, EOI, IPI send, ID read |
| `kernel/arch/x86_64/lapic.h` | **New.** LAPIC API declarations |
| `kernel/arch/x86_64/ioapic.c` | **New.** I/O APIC init, IRQ routing |
| `kernel/arch/x86_64/ioapic.h` | **New.** I/O APIC API declarations |
| `kernel/arch/x86_64/acpi.c` | **Modify.** Parse MADT type 0 (LAPIC), type 1 (I/O APIC), type 2 (ISO) |
| `kernel/arch/x86_64/acpi.h` | **Modify.** Export MADT parse results |
| `kernel/arch/x86_64/idt.c` | **Modify.** Expand to 256 entries, dual PIC/LAPIC EOI path |
| `kernel/arch/x86_64/arch.h` | **Modify.** Add `arch_irq_save`/`arch_irq_restore`, `arch_pause` |
| `kernel/core/main.c` | **Modify.** Call lapic_init, ioapic_init after acpi_init |
| `kernel/mm/pmm.c` | **Modify.** Add `pmm_lock` spinlock |
| `kernel/mm/kva.c` | **Modify.** Add `kva_lock` spinlock |
| `kernel/mm/vmm.c` | **Modify.** Add `vmm_window_lock` spinlock |
| `kernel/sched/sched.c` | **Modify.** Add `sched_lock` spinlock |
| `kernel/fs/pipe.c` | **Modify.** Add per-pipe spinlock |
| `kernel/fs/pty.c` | **Modify.** Add per-PTY spinlock |
| `kernel/fs/vfs.c` | **Modify.** Add ext2 pool lock |
| `kernel/fs/ext2.c` | **Modify.** Add `ext2_lock` spinlock |
| `kernel/syscall/futex.c` | **Modify.** Add `futex_lock` spinlock |
| `kernel/proc/proc.c` | **Modify.** Add `pid_lock` spinlock |
| `kernel/core/random.c` | **Modify.** Add `rng_lock` spinlock |
| `kernel/drivers/nvme.c` | **Modify.** Add `nvme_lock` spinlock |
| `kernel/net/tcp.c` | **Modify.** Add `tcp_lock` spinlock |
| `kernel/net/udp.c` | **Modify.** Add `udp_lock` spinlock |
| `kernel/net/socket.c` | **Modify.** Add `sock_lock` spinlock |
| `kernel/net/ip.c` | **Modify.** Add `ip_lock` spinlock |
| `kernel/net/eth.c` | **Modify.** Add `arp_lock` spinlock |
| `kernel/net/netdev.c` | **Modify.** Add `netdev_lock` spinlock |
| `kernel/net/epoll.c` | **Modify.** Add `epoll_lock` spinlock |
| `kernel/fs/blkdev.c` | **Modify.** Add `blkdev_lock` spinlock |
| `kernel/fs/ramfs.c` | **Modify.** Add per-instance spinlock |
| `Makefile` | **Modify.** Add lapic.c, ioapic.c to sources |
| `tests/expected/boot.txt` | **Modify.** Add LAPIC/SMP lines |

---

### Task 1: Spinlock Infrastructure + arch_irq_save/restore

**Files:**
- Create: `kernel/core/spinlock.h`
- Modify: `kernel/arch/x86_64/arch.h`

- [ ] **Step 1: Add `arch_irq_save`/`arch_irq_restore`/`arch_pause` to `arch.h`**

After the existing `arch_enable_irq`/`arch_disable_irq` definitions, add:

```c
/* Save interrupt flags and disable interrupts.
 * Returns the previous RFLAGS value (for restoring later). */
static inline unsigned long
arch_irq_save(void)
{
    unsigned long flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

/* Restore interrupt flags to a previously saved state. */
static inline void
arch_irq_restore(unsigned long flags)
{
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
}

/* Hint to the CPU that we are in a spin loop. Reduces power and improves
 * SMT performance. */
static inline void
arch_pause(void)
{
    __asm__ volatile("pause");
}
```

- [ ] **Step 2: Create `kernel/core/spinlock.h`**

```c
/* spinlock.h — Ticket spinlock for SMP synchronization
 *
 * Ticket lock guarantees FIFO ordering (no starvation).
 * Two 16-bit counters: 'next' (take-a-number) and 'owner' (now-serving).
 * A lock is free when owner == next.
 *
 * IRQ-safe variants save/restore interrupt flags via arch.h helpers
 * to prevent deadlock (ISR acquires lock already held by interrupted code).
 */
#ifndef AEGIS_SPINLOCK_H
#define AEGIS_SPINLOCK_H

#include "arch.h"

typedef struct {
    volatile uint16_t owner;
    volatile uint16_t next;
} spinlock_t;

#define SPINLOCK_INIT { 0, 0 }

static inline void
spin_lock(spinlock_t *lock)
{
    uint16_t ticket = __atomic_fetch_add(&lock->next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&lock->owner, __ATOMIC_ACQUIRE) != ticket)
        arch_pause();
}

static inline void
spin_unlock(spinlock_t *lock)
{
    __atomic_store_n(&lock->owner, lock->owner + 1, __ATOMIC_RELEASE);
}

static inline int
spin_trylock(spinlock_t *lock)
{
    uint16_t owner = __atomic_load_n(&lock->owner, __ATOMIC_RELAXED);
    uint16_t next  = __atomic_load_n(&lock->next, __ATOMIC_RELAXED);
    if (owner != next) return 0;
    return __atomic_compare_exchange_n(&lock->next, &next, (uint16_t)(next + 1),
                                       0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

/* IRQ-safe variants: disable interrupts before acquiring to prevent
 * deadlock if an ISR tries to acquire the same lock. */
typedef unsigned long irqflags_t;

static inline irqflags_t
spin_lock_irqsave(spinlock_t *lock)
{
    irqflags_t flags = arch_irq_save();
    spin_lock(lock);
    return flags;
}

static inline void
spin_unlock_irqrestore(spinlock_t *lock, irqflags_t flags)
{
    spin_unlock(lock);
    arch_irq_restore(flags);
}

#endif /* AEGIS_SPINLOCK_H */
```

- [ ] **Step 3: Verify build**

Run: `make clean && make`
Expected: Compiles. spinlock.h is header-only; no .c file needed.

- [ ] **Step 4: Commit**

```bash
git add kernel/core/spinlock.h kernel/arch/x86_64/arch.h
git commit -m "feat(phase38a): ticket spinlock infrastructure + arch_irq_save/restore"
```

---

### Task 2: MADT Parsing — LAPIC, I/O APIC, and Interrupt Source Overrides

**Files:**
- Modify: `kernel/arch/x86_64/acpi.c`
- Modify: `kernel/arch/x86_64/acpi.h`

Currently `scan_table()` just sets `g_madt_found = 1` when it sees the MADT. We need to actually parse the MADT entries.

- [ ] **Step 1: Add MADT data structures to `acpi.h`**

Add after existing declarations:

```c
/* SMP CPU info parsed from MADT */
#define SMP_MAX_CPUS 16

typedef struct {
    uint8_t apic_id;
    uint8_t enabled;
} smp_cpu_t;

extern smp_cpu_t  g_smp_cpus[SMP_MAX_CPUS];
extern uint32_t   g_smp_cpu_count;
extern uint8_t    g_bsp_apic_id;

/* I/O APIC info from MADT */
extern uint64_t   g_ioapic_addr;      /* physical address (0 = not found) */
extern uint32_t   g_ioapic_gsi_base;

/* Interrupt Source Overrides from MADT */
#define MADT_MAX_ISO 16
typedef struct {
    uint8_t  bus;          /* always 0 (ISA) */
    uint8_t  source_irq;   /* ISA IRQ number */
    uint32_t gsi;          /* Global System Interrupt (I/O APIC pin) */
    uint16_t flags;        /* polarity + trigger mode */
} madt_iso_t;

extern madt_iso_t g_madt_iso[MADT_MAX_ISO];
extern uint32_t   g_madt_iso_count;
```

- [ ] **Step 2: Add MADT parsing function to `acpi.c`**

Add a `parse_madt()` function before `scan_table()`:

```c
/* MADT global state */
smp_cpu_t  g_smp_cpus[SMP_MAX_CPUS];
uint32_t   g_smp_cpu_count = 0;
uint8_t    g_bsp_apic_id   = 0;
uint64_t   g_ioapic_addr   = 0;
uint32_t   g_ioapic_gsi_base = 0;
madt_iso_t g_madt_iso[MADT_MAX_ISO];
uint32_t   g_madt_iso_count = 0;

static void
parse_madt(uint64_t phys)
{
    uint32_t length = phys_read32(phys + 4);
    /* Local APIC address at MADT offset 36 (4 bytes) */
    /* (we use MSR-based LAPIC address instead, but record it) */

    /* MADT entries start at offset 44 */
    uint64_t off = 44;
    while (off + 2 <= length) {
        uint8_t type   = phys_read8(phys + off);
        uint8_t elen   = phys_read8(phys + off + 1);
        if (elen < 2) break;

        if (type == 0 && elen >= 8) {
            /* Type 0: Processor Local APIC */
            uint8_t apic_id = phys_read8(phys + off + 3);
            uint32_t flags  = phys_read32(phys + off + 4);
            if ((flags & 1) && g_smp_cpu_count < SMP_MAX_CPUS) {
                g_smp_cpus[g_smp_cpu_count].apic_id = apic_id;
                g_smp_cpus[g_smp_cpu_count].enabled  = 1;
                g_smp_cpu_count++;
            }
        } else if (type == 1 && elen >= 12) {
            /* Type 1: I/O APIC */
            if (g_ioapic_addr == 0) {
                uint32_t addr = phys_read32(phys + off + 4);
                g_ioapic_addr     = (uint64_t)addr;
                g_ioapic_gsi_base = phys_read32(phys + off + 8);
            }
        } else if (type == 2 && elen >= 10) {
            /* Type 2: Interrupt Source Override */
            if (g_madt_iso_count < MADT_MAX_ISO) {
                madt_iso_t *iso = &g_madt_iso[g_madt_iso_count];
                iso->bus        = phys_read8(phys + off + 2);
                iso->source_irq = phys_read8(phys + off + 3);
                iso->gsi        = phys_read32(phys + off + 4);
                iso->flags      = (uint16_t)phys_read32(phys + off + 8);
                g_madt_iso_count++;
            }
        }

        off += elen;
    }
}
```

- [ ] **Step 3: Wire parse_madt into scan_table**

Replace the existing MADT line in `scan_table()`:

```c
    else if (__builtin_memcmp(sig, "APIC", 4) == 0) {
        g_madt_found = 1;
        parse_madt(phys);
    }
```

- [ ] **Step 4: Detect BSP APIC ID**

At the end of `acpi_init()`, after the XSDT/RSDT scan loop (before the printk), add:

```c
    /* Determine BSP's APIC ID via CPUID leaf 1, EBX[31:24] */
    {
        uint32_t eax, ebx, ecx, edx;
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
        g_bsp_apic_id = (uint8_t)(ebx >> 24);
    }
```

- [ ] **Step 5: Update the acpi_init printk to report CPU count**

Replace the existing ACPI OK printk lines with:

```c
    if (g_mcfg_base != 0)
        printk("[ACPI] OK: MCFG+MADT parsed, %u CPUs\n",
               (unsigned)g_smp_cpu_count);
    else if (g_madt_found)
        printk("[ACPI] OK: MADT parsed, %u CPUs, no MCFG (legacy machine)\n",
               (unsigned)g_smp_cpu_count);
    else
        printk("[ACPI] OK: no MADT, no MCFG (legacy machine)\n");
```

- [ ] **Step 6: Update `tests/expected/boot.txt`**

The ACPI line changes. On `-machine pc` QEMU, SeaBIOS provides a MADT with 1 CPU. Replace:

```
[ACPI] OK: MADT parsed, no MCFG (legacy machine)
```

with:

```
[ACPI] OK: MADT parsed, 1 CPUs, no MCFG (legacy machine)
```

- [ ] **Step 7: Verify build + boot oracle**

Run: `make clean && make test`
Expected: EXIT 0 with updated boot.txt line.

- [ ] **Step 8: Commit**

```bash
git add kernel/arch/x86_64/acpi.c kernel/arch/x86_64/acpi.h tests/expected/boot.txt
git commit -m "feat(phase38a): parse MADT LAPIC, I/O APIC, and ISO entries"
```

---

### Task 3: LAPIC Driver

**Files:**
- Create: `kernel/arch/x86_64/lapic.c`
- Create: `kernel/arch/x86_64/lapic.h`
- Modify: `Makefile` (add lapic.c to sources)

- [ ] **Step 1: Create `kernel/arch/x86_64/lapic.h`**

```c
/* lapic.h — Local APIC driver for x86-64 SMP */
#ifndef AEGIS_LAPIC_H
#define AEGIS_LAPIC_H

#include <stdint.h>

/* Initialize the BSP's Local APIC. Maps MMIO, enables SVR.
 * Prints [LAPIC] OK line. Safe to call when no LAPIC present (no-op). */
void lapic_init(void);

/* Read this CPU's LAPIC ID (bits [31:24] of LAPIC_ID register). */
uint8_t lapic_id(void);

/* Send End-of-Interrupt to the Local APIC. Must be called at the end
 * of every interrupt handler when using APIC interrupt routing. */
void lapic_eoi(void);

/* Send a fixed-delivery IPI to a specific APIC ID. */
void lapic_send_ipi(uint8_t dest_apic_id, uint8_t vector);

/* Send an IPI to all CPUs except self. */
void lapic_send_ipi_all_excl_self(uint8_t vector);

/* Returns 1 if the LAPIC is active (initialized successfully). */
int lapic_active(void);

#endif /* AEGIS_LAPIC_H */
```

- [ ] **Step 2: Create `kernel/arch/x86_64/lapic.c`**

```c
/* lapic.c — Local APIC driver
 *
 * The Local APIC (LAPIC) is the per-CPU interrupt controller on x86-64.
 * Each CPU has its own LAPIC at the same physical address (typically
 * 0xFEE00000). The LAPIC handles:
 *   - End of Interrupt (EOI) — replaces PIC EOI on APIC systems
 *   - Inter-Processor Interrupts (IPIs) — for SMP communication
 *   - Local timer — per-CPU scheduling tick (Phase 38c)
 *   - Spurious interrupt vector
 */
#include "lapic.h"
#include "arch.h"
#include "acpi.h"
#include "../mm/kva.h"
#include "../mm/vmm.h"
#include "../core/printk.h"
#include <stdint.h>

/* LAPIC register offsets (byte offsets into MMIO region) */
#define LAPIC_ID        0x020
#define LAPIC_VER       0x030
#define LAPIC_TPR       0x080
#define LAPIC_EOI_REG   0x0B0
#define LAPIC_SVR       0x0F0
#define LAPIC_ICR_LOW   0x300
#define LAPIC_ICR_HIGH  0x310

/* SVR bits */
#define LAPIC_SVR_ENABLE  (1u << 8)
#define LAPIC_SVR_VECTOR  0xFF    /* spurious vector = 0xFF */

/* ICR delivery status bit */
#define ICR_DELIVERY_PENDING  (1u << 12)

/* IA32_APIC_BASE MSR */
#define IA32_APIC_BASE_MSR  0x1B

static volatile uint32_t *s_lapic = NULL;
static int s_lapic_active = 0;

static inline uint32_t
lapic_read(uint32_t off)
{
    return s_lapic[off / 4];
}

static inline void
lapic_write(uint32_t off, uint32_t val)
{
    s_lapic[off / 4] = val;
}

void
lapic_init(void)
{
    uint32_t lo, hi;
    uint64_t apic_base_phys;

    /* Read IA32_APIC_BASE MSR */
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(IA32_APIC_BASE_MSR));
    apic_base_phys = ((uint64_t)hi << 32) | (lo & 0xFFFFF000u);

    if (apic_base_phys == 0) {
        printk("[LAPIC] WARN: no LAPIC detected\n");
        return;
    }

    /* Map LAPIC MMIO page into kernel VA (uncached) */
    {
        uintptr_t va = (uintptr_t)kva_alloc_pages(1);
        /* Unmap the PMM-backed page, remap to LAPIC physical */
        vmm_unmap_page(va);
        vmm_map_page(va, apic_base_phys,
                     VMM_FLAG_WRITABLE | VMM_FLAG_WC | VMM_FLAG_UCMINUS);
        s_lapic = (volatile uint32_t *)va;
    }

    /* Enable LAPIC: set SVR with enable bit + spurious vector 0xFF */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | LAPIC_SVR_VECTOR);

    /* Clear Task Priority Register — accept all interrupt priorities */
    lapic_write(LAPIC_TPR, 0);

    s_lapic_active = 1;

    printk("[LAPIC] OK: id=%u base=0x%lx\n",
           (unsigned)lapic_id(), apic_base_phys);
}

uint8_t
lapic_id(void)
{
    if (!s_lapic) return 0;
    return (uint8_t)(lapic_read(LAPIC_ID) >> 24);
}

void
lapic_eoi(void)
{
    if (s_lapic)
        lapic_write(LAPIC_EOI_REG, 0);
}

void
lapic_send_ipi(uint8_t dest_apic_id, uint8_t vector)
{
    if (!s_lapic) return;

    /* Wait for previous IPI to complete */
    while (lapic_read(LAPIC_ICR_LOW) & ICR_DELIVERY_PENDING)
        arch_pause();

    lapic_write(LAPIC_ICR_HIGH, ((uint32_t)dest_apic_id << 24));
    lapic_write(LAPIC_ICR_LOW, (uint32_t)vector);
}

void
lapic_send_ipi_all_excl_self(uint8_t vector)
{
    if (!s_lapic) return;

    while (lapic_read(LAPIC_ICR_LOW) & ICR_DELIVERY_PENDING)
        arch_pause();

    lapic_write(LAPIC_ICR_HIGH, 0);
    /* Shorthand = 11 (all-excluding-self) in bits [19:18] */
    lapic_write(LAPIC_ICR_LOW, (uint32_t)vector | (0x3u << 18));
}

int
lapic_active(void)
{
    return s_lapic_active;
}
```

- [ ] **Step 3: Add lapic.c to Makefile ARCH_SRCS**

Find the line with `kernel/arch/x86_64/ps2_mouse.c` and add after it:

```
    kernel/arch/x86_64/lapic.c \
```

- [ ] **Step 4: Verify build**

Run: `make clean && make`
Expected: Compiles without errors.

- [ ] **Step 5: Commit**

```bash
git add kernel/arch/x86_64/lapic.c kernel/arch/x86_64/lapic.h Makefile
git commit -m "feat(phase38a): LAPIC driver — init, EOI, IPI"
```

---

### Task 4: I/O APIC Driver

**Files:**
- Create: `kernel/arch/x86_64/ioapic.c`
- Create: `kernel/arch/x86_64/ioapic.h`
- Modify: `Makefile`

- [ ] **Step 1: Create `kernel/arch/x86_64/ioapic.h`**

```c
/* ioapic.h — I/O APIC driver for SMP interrupt routing */
#ifndef AEGIS_IOAPIC_H
#define AEGIS_IOAPIC_H

#include <stdint.h>

/* Initialize the I/O APIC. Maps MMIO, routes legacy IRQs.
 * Disables the 8259A PIC. Safe to skip if g_ioapic_addr == 0. */
void ioapic_init(void);

/* Route an IRQ pin to a vector, targeting a specific LAPIC ID.
 * flags: bit 1 = active-low polarity, bit 3 = level-triggered. */
void ioapic_route_irq(uint8_t pin, uint8_t vector,
                      uint8_t dest_apic_id, uint16_t flags);

/* Mask (disable) an I/O APIC pin. */
void ioapic_mask(uint8_t pin);

/* Unmask (enable) an I/O APIC pin. */
void ioapic_unmask(uint8_t pin);

/* Returns 1 if I/O APIC is active. */
int ioapic_active(void);

#endif /* AEGIS_IOAPIC_H */
```

- [ ] **Step 2: Create `kernel/arch/x86_64/ioapic.c`**

```c
/* ioapic.c — I/O APIC interrupt routing
 *
 * The I/O APIC replaces the 8259A PIC for routing external interrupts
 * to LAPIC(s) on SMP systems. It has 24 redirection table entries,
 * each mapping an input pin to a vector + destination CPU.
 *
 * On legacy -machine pc (no I/O APIC in MADT), this driver skips
 * initialization and the kernel continues using the 8259A PIC.
 */
#include "ioapic.h"
#include "lapic.h"
#include "acpi.h"
#include "arch.h"
#include "../mm/kva.h"
#include "../mm/vmm.h"
#include "../core/printk.h"
#include <stdint.h>

#define IOAPIC_REGSEL  0x00
#define IOAPIC_REGWIN  0x10

/* Registers */
#define IOAPIC_ID      0x00
#define IOAPIC_VER     0x01
#define IOAPIC_REDTBL  0x10  /* entries at 0x10 + 2*N (lo) and 0x10 + 2*N + 1 (hi) */

static volatile uint32_t *s_ioapic = NULL;
static int s_ioapic_active = 0;

static uint32_t
ioapic_read(uint8_t reg)
{
    s_ioapic[IOAPIC_REGSEL / 4] = (uint32_t)reg;
    return s_ioapic[IOAPIC_REGWIN / 4];
}

static void
ioapic_write(uint8_t reg, uint32_t val)
{
    s_ioapic[IOAPIC_REGSEL / 4] = (uint32_t)reg;
    s_ioapic[IOAPIC_REGWIN / 4] = val;
}

void
ioapic_route_irq(uint8_t pin, uint8_t vector,
                 uint8_t dest_apic_id, uint16_t flags)
{
    if (!s_ioapic) return;

    uint32_t lo = (uint32_t)vector;
    /* Polarity: bit 13 (0=active high, 1=active low) */
    if (flags & 0x02) lo |= (1u << 13);
    /* Trigger mode: bit 15 (0=edge, 1=level) */
    if (flags & 0x08) lo |= (1u << 15);

    uint32_t hi = ((uint32_t)dest_apic_id << 24);

    uint8_t reg_lo = (uint8_t)(IOAPIC_REDTBL + pin * 2);
    uint8_t reg_hi = (uint8_t)(IOAPIC_REDTBL + pin * 2 + 1);

    ioapic_write(reg_hi, hi);
    ioapic_write(reg_lo, lo);
}

void
ioapic_mask(uint8_t pin)
{
    if (!s_ioapic) return;
    uint8_t reg_lo = (uint8_t)(IOAPIC_REDTBL + pin * 2);
    uint32_t lo = ioapic_read(reg_lo);
    lo |= (1u << 16);  /* set mask bit */
    ioapic_write(reg_lo, lo);
}

void
ioapic_unmask(uint8_t pin)
{
    if (!s_ioapic) return;
    uint8_t reg_lo = (uint8_t)(IOAPIC_REDTBL + pin * 2);
    uint32_t lo = ioapic_read(reg_lo);
    lo &= ~(1u << 16);  /* clear mask bit */
    ioapic_write(reg_lo, lo);
}

/* Disable the 8259A PIC by masking all IRQs and remapping to 0xF0-0xFF. */
static void
pic_disable(void)
{
    /* Remap PIC to 0xF0-0xFF so spurious interrupts don't conflict */
    outb(0x20, 0x11); outb(0xA0, 0x11);  /* ICW1 */
    outb(0x21, 0xF0); outb(0xA1, 0xF8);  /* ICW2: vectors 0xF0/0xF8 */
    outb(0x21, 0x04); outb(0xA1, 0x02);  /* ICW3: cascade */
    outb(0x21, 0x01); outb(0xA1, 0x01);  /* ICW4 */
    outb(0x21, 0xFF); outb(0xA1, 0xFF);  /* mask all */
}

void
ioapic_init(void)
{
    uint32_t i, ver, max_redir;

    if (g_ioapic_addr == 0) {
        /* No I/O APIC in MADT — continue using 8259A PIC */
        return;
    }

    /* Map I/O APIC MMIO page */
    {
        uintptr_t va = (uintptr_t)kva_alloc_pages(1);
        vmm_unmap_page(va);
        vmm_map_page(va, g_ioapic_addr,
                     VMM_FLAG_WRITABLE | VMM_FLAG_WC | VMM_FLAG_UCMINUS);
        s_ioapic = (volatile uint32_t *)va;
    }

    /* Read version register to get max redirection entries */
    ver = ioapic_read(IOAPIC_VER);
    max_redir = ((ver >> 16) & 0xFF) + 1;

    /* Mask all I/O APIC entries first */
    for (i = 0; i < max_redir; i++)
        ioapic_mask((uint8_t)i);

    /* Disable the 8259A PIC */
    pic_disable();

    /* Route legacy ISA IRQs through the I/O APIC.
     * Check MADT Interrupt Source Overrides for pin remapping. */
    {
        uint8_t bsp = g_bsp_apic_id;

        /* Default: ISA IRQ N maps to I/O APIC pin N, edge-triggered, active-high */
        /* PIT: IRQ 0 → often remapped to pin 2 via ISO */
        uint8_t pit_pin = 0;
        uint16_t pit_flags = 0;
        uint8_t sci_pin = (uint8_t)acpi_get_sci_irq();
        uint16_t sci_flags = 0;

        /* Check ISOs for remappings */
        for (i = 0; i < g_madt_iso_count; i++) {
            if (g_madt_iso[i].source_irq == 0) {
                pit_pin   = (uint8_t)g_madt_iso[i].gsi;
                pit_flags = g_madt_iso[i].flags;
            }
            if (g_madt_iso[i].source_irq == acpi_get_sci_irq()) {
                sci_pin   = (uint8_t)g_madt_iso[i].gsi;
                sci_flags = g_madt_iso[i].flags;
            }
        }

        /* Route active IRQs to BSP */
        ioapic_route_irq(pit_pin, 0x20, bsp, pit_flags);   /* PIT → vector 0x20 */
        ioapic_route_irq(1,       0x21, bsp, 0);             /* Keyboard → 0x21 */
        ioapic_route_irq(12,      0x2C, bsp, 0);             /* PS/2 mouse → 0x2C */
        if (sci_pin != 0)
            ioapic_route_irq(sci_pin, (uint8_t)(0x20 + acpi_get_sci_irq()),
                             bsp, sci_flags);                 /* ACPI SCI */
    }

    s_ioapic_active = 1;

    printk("[IOAPIC] OK: %u pins, PIC disabled\n", max_redir);
}

int
ioapic_active(void)
{
    return s_ioapic_active;
}
```

- [ ] **Step 3: Add ioapic.c to Makefile**

After the lapic.c line, add:

```
    kernel/arch/x86_64/ioapic.c \
```

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/ioapic.c kernel/arch/x86_64/ioapic.h Makefile
git commit -m "feat(phase38a): I/O APIC driver — IRQ routing, PIC disable"
```

---

### Task 5: IDT Expansion + Dual PIC/LAPIC EOI

**Files:**
- Modify: `kernel/arch/x86_64/idt.c`
- Modify: `kernel/arch/x86_64/isr.asm` (if IDT stub count needs increasing)
- Modify: `kernel/core/main.c`

- [ ] **Step 1: Expand IDT to 256 entries**

In `idt.c`, change:
```c
static aegis_idt_gate_t s_idt[48];
```
to:
```c
static aegis_idt_gate_t s_idt[256];
```

Update `idt_init()` to install gates for the additional vectors. The existing 48 stubs (0x00-0x2F) remain. Add a gate for vector 0xFF (LAPIC spurious — points to a no-op handler that just does `iretq`).

- [ ] **Step 2: Add dual EOI path in `isr_dispatch`**

Add `#include "lapic.h"` and `#include "ioapic.h"` to idt.c.

Replace the current EOI block (the `pic_send_eoi` call and spurious check) with:

```c
        /* Send EOI: LAPIC if active, otherwise PIC */
        if (lapic_active()) {
            lapic_eoi();
        } else {
            /* Legacy PIC EOI with spurious IRQ guard */
            if ((irq == 7 || irq == 15) && !pic_irq_is_real(irq)) {
                if (irq == 15)
                    outb(0x20, 0x20);
            } else {
                pic_send_eoi(irq);
            }
        }
```

- [ ] **Step 3: Wire LAPIC + I/O APIC init into main.c**

In `kernel/core/main.c`, add includes for `lapic.h` and `ioapic.h`. After `acpi_init()`, add:

```c
    lapic_init();           /* Local APIC — [LAPIC] OK                       */
    ioapic_init();          /* I/O APIC — [IOAPIC] OK or silent skip         */
```

- [ ] **Step 4: Update boot.txt**

Add after the existing ACPI lines:

```
[LAPIC] OK: id=0 base=0xfee00000
```

Note: I/O APIC does NOT print on `-machine pc` (no I/O APIC present), so no `[IOAPIC]` line in boot.txt.

- [ ] **Step 5: Verify build + boot oracle**

Run: `make clean && make test`
Expected: EXIT 0. The LAPIC init succeeds even on `-machine pc` (CPU has a LAPIC; only I/O APIC is missing).

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/x86_64/idt.c kernel/core/main.c tests/expected/boot.txt
git commit -m "feat(phase38a): IDT 256 entries, dual PIC/LAPIC EOI, LAPIC+IOAPIC init"
```

---

### Task 6: Add Spinlocks to Core Kernel Subsystems

**Files:**
- Modify: `kernel/mm/pmm.c`
- Modify: `kernel/mm/kva.c`
- Modify: `kernel/mm/vmm.c`
- Modify: `kernel/sched/sched.c`
- Modify: `kernel/proc/proc.c`
- Modify: `kernel/core/random.c`
- Modify: `kernel/syscall/futex.c`

This task is mechanical: add `#include "spinlock.h"`, declare a static spinlock, wrap critical sections with `spin_lock_irqsave`/`spin_unlock_irqrestore`.

- [ ] **Step 1: PMM spinlock**

In `kernel/mm/pmm.c`, add near the top:
```c
#include "spinlock.h"
static spinlock_t pmm_lock = SPINLOCK_INIT;
```

Wrap `pmm_alloc_page()`:
```c
uint64_t pmm_alloc_page(void) {
    irqflags_t flags = spin_lock_irqsave(&pmm_lock);
    /* ... existing bitmap scan ... */
    spin_unlock_irqrestore(&pmm_lock, flags);
    return result;
}
```

Same for `pmm_free_page()`.

- [ ] **Step 2: KVA spinlock**

In `kernel/mm/kva.c`:
```c
#include "spinlock.h"
static spinlock_t kva_lock = SPINLOCK_INIT;
```

Wrap `kva_alloc_pages()` — the entire function body (from `base = s_kva_next` through the loop to return).

- [ ] **Step 3: VMM window spinlock**

In `kernel/mm/vmm.c`:
```c
#include "spinlock.h"
static spinlock_t vmm_window_lock = SPINLOCK_INIT;
```

Wrap `vmm_window_map()`/`vmm_window_unmap()` and all functions that use them (`vmm_map_page`, `vmm_unmap_page`, `vmm_phys_of`, `alloc_table`).

**Note:** This is the most invasive lock because the window functions are called from many places. The lock must be held for the entire page-table walk (map → read → overwrite PTE → read → ... → unmap).

- [ ] **Step 4: Scheduler spinlock**

In `kernel/sched/sched.c`:
```c
#include "spinlock.h"
static spinlock_t sched_lock = SPINLOCK_INIT;
```

Wrap: `sched_spawn()`, `sched_add()`, `sched_block()`, `sched_wake()`, `sched_exit()`, `sched_tick()`, `sched_stop()`, `sched_resume()`.

**Critical:** `sched_tick()` is called from the PIT ISR (interrupts already disabled). Use `spin_lock`/`spin_unlock` (not irqsave) inside ISR context. For non-ISR callers (sched_block, sched_exit, etc.), use `spin_lock_irqsave`.

- [ ] **Step 5: PID, futex, RNG spinlocks**

`kernel/proc/proc.c`:
```c
#include "spinlock.h"
static spinlock_t pid_lock = SPINLOCK_INIT;
```
Wrap `proc_alloc_pid()`.

`kernel/syscall/futex.c`:
```c
#include "spinlock.h"
static spinlock_t futex_lock = SPINLOCK_INIT;
```
Wrap `futex_wake_addr()` and the FUTEX_WAIT path in `sys_futex()`.

`kernel/core/random.c`:
```c
#include "spinlock.h"
static spinlock_t rng_lock = SPINLOCK_INIT;
```
Wrap `random_get_bytes()` (which calls `rekey()` and `refill_buf()`). Note: `random_add_interrupt_entropy()` is called from ISR — it only writes `s_entropy_acc` which is read by `rekey()`. Use irqsave around the rekey path.

- [ ] **Step 6: Verify build + boot oracle**

Run: `make clean && make test`
Expected: EXIT 0. Single-core, locks never contended.

- [ ] **Step 7: Commit**

```bash
git add kernel/mm/pmm.c kernel/mm/kva.c kernel/mm/vmm.c kernel/sched/sched.c \
        kernel/proc/proc.c kernel/syscall/futex.c kernel/core/random.c
git commit -m "feat(phase38a): spinlocks for PMM, KVA, VMM, scheduler, PID, futex, RNG"
```

---

### Task 7: Add Spinlocks to Filesystem + Network + Drivers

**Files:**
- Modify: `kernel/fs/pipe.c`, `kernel/fs/pty.c`, `kernel/fs/ext2.c`, `kernel/fs/blkdev.c`, `kernel/fs/ramfs.c`, `kernel/fs/vfs.c`
- Modify: `kernel/net/tcp.c`, `kernel/net/udp.c`, `kernel/net/socket.c`, `kernel/net/ip.c`, `kernel/net/eth.c`, `kernel/net/netdev.c`, `kernel/net/epoll.c`
- Modify: `kernel/drivers/nvme.c`

Same mechanical pattern: `#include "spinlock.h"`, declare lock, wrap critical sections.

- [ ] **Step 1: Pipe + PTY locks**

`pipe.c`: Add `spinlock_t lock` field to `pipe_t` struct (in pipe.h or inline). Initialize in `pipe_create()`. Wrap `pipe_read_fn()` and `pipe_write_fn()`.

`pty.c`: Add `spinlock_t lock` field to `pty_pair_t`. Wrap master/slave read/write functions.

- [ ] **Step 2: ext2 + blkdev + ramfs locks**

`ext2.c`: Global `ext2_lock`. Wrap all functions that read/write the superblock, BGDs, block cache, or allocate/free inodes.

`blkdev.c`: Global `blkdev_lock`. Wrap `blkdev_register()`, `blkdev_unregister()`.

`ramfs.c`: Per-instance `lock` field in `ramfs_t`. Wrap file create/read/write/unlink.

`vfs.c`: Wrap `s_ext2_pool` access with a lock (or use the existing ext2_lock).

- [ ] **Step 3: Network stack locks**

`tcp.c`: Global `tcp_lock`. Wrap `tcp_find()`, `tcp_alloc()`, `tcp_rx()`, `tcp_tick()`.

`udp.c`: Global `udp_lock`. Wrap binding table access.

`socket.c`: Global `sock_lock`. Wrap `sock_alloc()`, `sock_get()`, `sock_free()`.

`ip.c`: Global `ip_lock`. Wrap `s_my_ip`/`s_netmask`/`s_gateway` access and shared buffer use.

`eth.c`: Global `arp_lock`. Wrap ARP table access and shared TX buffer.

`netdev.c`: Global `netdev_lock`. Wrap device registry.

`epoll.c`: Global `epoll_lock`. Wrap epoll instance table.

- [ ] **Step 4: NVMe lock**

`nvme.c`: Global `nvme_lock`. Wrap all queue operations: admin submit/completion, I/O submit/completion, bounce buffer access.

- [ ] **Step 5: Verify build + boot oracle**

Run: `make clean && make test`
Expected: EXIT 0.

- [ ] **Step 6: Commit**

```bash
git add kernel/fs/pipe.c kernel/fs/pty.c kernel/fs/ext2.c kernel/fs/blkdev.c \
        kernel/fs/ramfs.c kernel/fs/vfs.c \
        kernel/net/tcp.c kernel/net/udp.c kernel/net/socket.c kernel/net/ip.c \
        kernel/net/eth.c kernel/net/netdev.c kernel/net/epoll.c \
        kernel/drivers/nvme.c
git commit -m "feat(phase38a): spinlocks for filesystem, network, and drivers"
```

---

### Task 8: Integration Test

**Files:**
- No new files.

- [ ] **Step 1: Run boot oracle**

Run: `make test`
Expected: EXIT 0.

- [ ] **Step 2: Run test_integrated.py on q35**

This exercises the I/O APIC path (q35 has an I/O APIC). All 25 tests must pass (DHCP timing failure is acceptable).

- [ ] **Step 3: Commit any fixes**

If test failures required fixes, commit them.

---

## Self-Review

**1. Spec coverage (38a section):**
- [x] LAPIC driver → Task 3
- [x] I/O APIC driver → Task 4
- [x] MADT parsing (LAPIC + I/O APIC + ISO entries) → Task 2
- [x] PIC → APIC transition → Task 4 (pic_disable), Task 5 (dual EOI)
- [x] Spinlock infrastructure → Task 1
- [x] Core kernel locks (PMM, KVA, VMM, scheduler, PID, futex, RNG) → Task 6
- [x] Filesystem locks (pipe, PTY, ext2, blkdev, ramfs) → Task 7
- [x] Network locks (TCP, UDP, socket, IP, ARP, netdev, epoll) → Task 7
- [x] Driver locks (NVMe) → Task 7
- [x] IDT expansion to 256 entries → Task 5
- [x] Graceful PIC fallback on `-machine pc` → Task 5 (dual EOI)
- [x] Boot oracle update → Tasks 2 + 5
- [x] Lock ordering → Spec (referenced, not re-implemented in plan)

**2. Placeholder scan:** No TBD/TODO. Tasks 6+7 describe the pattern but don't show every line — this is intentional because the pattern is identical across 25+ files. The implementer applies the same wrapping to each function.

**3. Type consistency:**
- `spinlock_t` / `SPINLOCK_INIT` / `spin_lock_irqsave` / `spin_unlock_irqrestore` consistent across all tasks ✓
- `lapic_active()` / `lapic_eoi()` used consistently in idt.c ✓
- `g_smp_cpus` / `g_ioapic_addr` / `g_madt_iso` match between acpi.h declarations and acpi.c definitions ✓
