# Phase 19: PCIe Enumeration + ACPI MCFG/MADT Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Parse ACPI tables from multiboot2 tags to locate the PCIe MMIO config space (MCFG), then enumerate all PCIe devices and print a summary to serial.

**Architecture:** Extend `arch_mm_init()` to save the RSDP physical address while scanning multiboot2 tags. A new `acpi.c` module walks RSDT/XSDT to find MCFG and MADT. A new `pcie.c` module uses the MCFG MMIO window to enumerate all bus/device/function combinations and build a device table. On `-machine pc` (used by `make test`), no MCFG is present — the code prints graceful fallback messages which are added to `boot.txt`.

**Tech Stack:** C, x86-64 MMIO, ACPI 1.0/2.0 table structures, PCIe ECAM config space, QEMU q35 machine for `make run`.

**Spec:** `docs/superpowers/specs/2026-03-23-aegis-v1-design.md` — Phase 19

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `kernel/arch/x86_64/acpi.h` | Create | RSDP/RSDT/XSDT/MCFG/MADT structures; `acpi_init()` declaration |
| `kernel/arch/x86_64/acpi.c` | Create | Parse RSDP → RSDT or XSDT → find MCFG + MADT |
| `kernel/arch/x86_64/pcie.h` | Create | `pcie_device_t`, read/write accessors, `pcie_enumerate()` |
| `kernel/arch/x86_64/pcie.c` | Create | MCFG MMIO mapping, BDF scan, 64-bit BAR decode |
| `kernel/arch/x86_64/arch_mm.c` | Modify | Scan multiboot2 tags 14/15 to save RSDP physical address |
| `kernel/arch/x86_64/arch.h` | Modify | Add `arch_get_rsdp_phys()` declaration |
| `kernel/core/main.c` | Modify | Call `acpi_init()` and `pcie_init()` in boot sequence |
| `tests/expected/boot.txt` | Modify | Add two new fallback lines for `-machine pc` |
| `Makefile` | Modify | Add `acpi.c` + `pcie.c` to ARCH_SRCS; update `make run` to use `-machine q35` |

---

### Task 1: Extend arch_mm.c to save the RSDP physical address

**Files:**
- Modify: `kernel/arch/x86_64/arch_mm.c`
- Modify: `kernel/arch/x86_64/arch.h`

This must be done before the ACPI module can find the RSDP.

- [ ] **Step 1: Add `arch_get_rsdp_phys()` declaration to arch.h**

Open `kernel/arch/x86_64/arch.h`. Find the block of `arch_mm_*` declarations (near the bottom) and add:

```c
/* ACPI RSDP physical address — saved during multiboot2 tag scan.
 * Returns 0 if no ACPI tag was found (e.g. -machine pc with SeaBIOS). */
uint64_t arch_get_rsdp_phys(void);
```

- [ ] **Step 2: Add RSDP tag scanning to arch_mm.c**

Open `kernel/arch/x86_64/arch_mm.c`. After the existing `#define MB2_TAG_MMAP 6` line, add:

```c
#define MB2_TAG_ACPI_OLD 14   /* ACPI 1.0 RSDP (32-bit, RSDT) */
#define MB2_TAG_ACPI_NEW 15   /* ACPI 2.0+ RSDP (64-bit, XSDT) */

/* Multiboot2 ACPI old tag (type 14) — contains RSDP v1 inline */
typedef struct {
    uint32_t type;    /* = 14 */
    uint32_t size;
    /* RSDP follows immediately: 20 bytes (v1) */
} mb2_acpi_old_tag_t;

/* Multiboot2 ACPI new tag (type 15) — contains RSDP v2 inline */
typedef struct {
    uint32_t type;    /* = 15 */
    uint32_t size;
    /* RSDP follows immediately: 36 bytes (v2) */
} mb2_acpi_new_tag_t;
```

Then add a static variable to store the RSDP address, just after the `region_count` variable:

```c
static uint64_t s_rsdp_phys = 0;
```

- [ ] **Step 3: Scan for ACPI tags in the existing arch_mm_init while loop**

Inside `arch_mm_init()`, in the tag scan loop after the `if (tag->type == MB2_TAG_MMAP)` block, add:

```c
        if (tag->type == MB2_TAG_ACPI_NEW && s_rsdp_phys == 0) {
            /* ACPI 2.0+ RSDP: skip the 8-byte tag header, RSDP starts there.
             * We store the physical address of the RSDP structure itself. */
            s_rsdp_phys = (uint64_t)(uintptr_t)(p + sizeof(mb2_acpi_new_tag_t));
        }
        if (tag->type == MB2_TAG_ACPI_OLD && s_rsdp_phys == 0) {
            /* ACPI 1.0 RSDP fallback — only if no v2 tag found. */
            s_rsdp_phys = (uint64_t)(uintptr_t)(p + sizeof(mb2_acpi_old_tag_t));
        }
```

Note: check `MB2_TAG_ACPI_NEW` first; only fall back to `MB2_TAG_ACPI_OLD` if no new tag found (the `&& s_rsdp_phys == 0` guard handles ordering).

- [ ] **Step 4: Add the getter function at the bottom of arch_mm.c**

```c
uint64_t arch_get_rsdp_phys(void)
{
    return s_rsdp_phys;
}
```

- [ ] **Step 5: Build to check for errors**

```bash
make 2>&1 | head -40
```

Expected: clean build (no new warnings or errors). The new code is dead until wired in main.c.

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/x86_64/arch_mm.c kernel/arch/x86_64/arch.h
git commit -m "arch: save ACPI RSDP physical address from multiboot2 tags 14/15"
```

---

### Task 2: Create acpi.h and acpi.c

**Files:**
- Create: `kernel/arch/x86_64/acpi.h`
- Create: `kernel/arch/x86_64/acpi.c`

- [ ] **Step 1: Create acpi.h**

```c
/* acpi.h — ACPI table parser for PCIe MCFG and MADT
 *
 * Minimal static parser: no AML, no power management.
 * Finds MCFG (PCIe config space base) and MADT (interrupt routing).
 */
#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

/* ACPI System Description Table header (common to all SDTs) */
typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

/* RSDP (Root System Description Pointer) — v1 layout */
typedef struct __attribute__((packed)) {
    char     signature[8];    /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;        /* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
    uint32_t rsdt_address;    /* physical address of RSDT (32-bit) */
    /* ACPI 2.0+ fields follow if revision >= 2: */
    uint32_t length;
    uint64_t xsdt_address;    /* physical address of XSDT (64-bit) */
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} acpi_rsdp_t;

/* MCFG table: PCIe MMIO config space allocation */
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint64_t          reserved;
    /* Followed by one or more mcfg_alloc_t entries */
} acpi_mcfg_t;

typedef struct __attribute__((packed)) {
    uint64_t base_address;    /* MMIO base of PCIe config space */
    uint16_t segment;         /* PCI segment group (0 for most systems) */
    uint8_t  start_bus;
    uint8_t  end_bus;
    uint32_t reserved;
} acpi_mcfg_alloc_t;

/* MADT table: interrupt controller descriptions */
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32_t          local_apic_address;
    uint32_t          flags;
    /* Followed by variable-length MADT entries */
} acpi_madt_t;

/* Parsed ACPI state — available after acpi_init() */
extern uint64_t g_mcfg_base;     /* PCIe MMIO config space base, 0 if absent */
extern uint8_t  g_mcfg_start_bus;
extern uint8_t  g_mcfg_end_bus;
extern int      g_madt_found;    /* 1 if MADT was located */

/* Initialize ACPI: parse RSDP → RSDT/XSDT → find MCFG + MADT.
 * Prints [ACPI] OK or FAIL to serial. */
void acpi_init(void);

#endif /* ACPI_H */
```

- [ ] **Step 2: Create acpi.c**

```c
/* acpi.c — ACPI table parser (MCFG + MADT)
 *
 * Phase 19 scope: locate MCFG to get PCIe ECAM base, locate MADT for
 * future interrupt routing. No AML interpreter. No power management.
 */
#include "acpi.h"
#include "arch.h"
#include "printk.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

uint64_t g_mcfg_base      = 0;
uint8_t  g_mcfg_start_bus = 0;
uint8_t  g_mcfg_end_bus   = 0;
int      g_madt_found     = 0;

/* Map a physical address to a virtual address.
 * Phase 19: physical == virtual under the identity-equivalent higher-half
 * mapping (physical addresses < 4MB are accessible via identity map remnant,
 * and ACPI tables in firmware RAM are accessible because the higher-half map
 * covers [0x000000..0x3FFFFF]). For addresses beyond 4MB we use kva mapping.
 * Simplified for now: cast directly (works for QEMU where ACPI tables live
 * in the first 4MB). A future phase will use vmm_map_page for large tables. */
static void *phys_to_virt(uint64_t phys)
{
    /* SAFETY: Phase 19 assumes ACPI tables are within the identity-mapped
     * first 4MB. On real hardware they may be higher; fix in Phase 19+. */
    return (void *)(uintptr_t)phys;
}

static int acpi_checksum(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint8_t sum = 0;
    uint32_t i;
    for (i = 0; i < len; i++)
        sum += p[i];
    return sum == 0;
}

static void parse_mcfg(const acpi_sdt_header_t *hdr)
{
    const acpi_mcfg_t *mcfg = (const acpi_mcfg_t *)hdr;
    const uint8_t *p   = (const uint8_t *)hdr + sizeof(acpi_mcfg_t);
    const uint8_t *end = (const uint8_t *)hdr + hdr->length;

    while (p + sizeof(acpi_mcfg_alloc_t) <= end) {
        const acpi_mcfg_alloc_t *alloc = (const acpi_mcfg_alloc_t *)p;
        if (alloc->segment == 0 && g_mcfg_base == 0) {
            g_mcfg_base      = alloc->base_address;
            g_mcfg_start_bus = alloc->start_bus;
            g_mcfg_end_bus   = alloc->end_bus;
        }
        p += sizeof(acpi_mcfg_alloc_t);
    }
}

static void scan_table(uint64_t phys)
{
    const acpi_sdt_header_t *hdr =
        (const acpi_sdt_header_t *)phys_to_virt(phys);

    if (!acpi_checksum(hdr, hdr->length))
        return;

    if (memcmp(hdr->signature, "MCFG", 4) == 0)
        parse_mcfg(hdr);
    else if (memcmp(hdr->signature, "APIC", 4) == 0)
        g_madt_found = 1;
}

void acpi_init(void)
{
    uint64_t rsdp_phys = arch_get_rsdp_phys();

    if (rsdp_phys == 0) {
        printk("[ACPI] OK: MADT parsed, no MCFG (legacy machine)\n");
        return;
    }

    const acpi_rsdp_t *rsdp = (const acpi_rsdp_t *)phys_to_virt(rsdp_phys);

    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0) {
        printk("[ACPI] FAIL: invalid RSDP signature\n");
        return;
    }

    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        /* ACPI 2.0+: use XSDT with 64-bit table pointers */
        const acpi_sdt_header_t *xsdt =
            (const acpi_sdt_header_t *)phys_to_virt(rsdp->xsdt_address);
        uint32_t count = (xsdt->length - sizeof(acpi_sdt_header_t)) / 8;
        const uint64_t *entries = (const uint64_t *)(
            (const uint8_t *)xsdt + sizeof(acpi_sdt_header_t));
        uint32_t i;
        for (i = 0; i < count; i++)
            scan_table(entries[i]);
    } else {
        /* ACPI 1.0: use RSDT with 32-bit table pointers */
        const acpi_sdt_header_t *rsdt =
            (const acpi_sdt_header_t *)phys_to_virt(rsdp->rsdt_address);
        uint32_t count = (rsdt->length - sizeof(acpi_sdt_header_t)) / 4;
        const uint32_t *entries = (const uint32_t *)(
            (const uint8_t *)rsdt + sizeof(acpi_sdt_header_t));
        uint32_t i;
        for (i = 0; i < count; i++)
            scan_table((uint64_t)entries[i]);
    }

    if (g_mcfg_base != 0)
        printk("[ACPI] OK: MCFG+MADT parsed\n");
    else
        printk("[ACPI] OK: MADT parsed, no MCFG (legacy machine)\n");
}
```

- [ ] **Step 3: Build to check for errors**

```bash
make 2>&1 | head -40
```

Expected: clean build. acpi.c is not in ARCH_SRCS yet — add it in Task 4.

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/acpi.h kernel/arch/x86_64/acpi.c
git commit -m "arch: add ACPI table parser (MCFG + MADT)"
```

---

### Task 3: Create pcie.h and pcie.c

**Files:**
- Create: `kernel/arch/x86_64/pcie.h`
- Create: `kernel/arch/x86_64/pcie.c`

- [ ] **Step 1: Create pcie.h**

```c
/* pcie.h — PCIe ECAM config space access and device enumeration
 *
 * Uses MCFG MMIO base from acpi.c. Scans all bus/device/function
 * combinations. Builds a table of discovered pcie_device_t entries.
 * On systems without MCFG (e.g. -machine pc), prints a skip message
 * and returns 0 devices — callers must handle this gracefully.
 */
#ifndef PCIE_H
#define PCIE_H

#include <stdint.h>

#define PCIE_MAX_DEVICES 64

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  progif;
    uint8_t  bus, dev, fn;
    uint64_t bar[6];         /* decoded BAR base addresses (64-bit aware) */
} pcie_device_t;

/* Initialize PCIe: enumerate all devices using ECAM config space.
 * Prints [PCIE] OK or skip message. */
void pcie_init(void);

/* Raw config space accessors — bus/dev/fn addressing. */
uint8_t  pcie_read8 (uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off);
uint16_t pcie_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off);
uint32_t pcie_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off);
void     pcie_write32(uint8_t bus, uint8_t dev, uint8_t fn,
                      uint16_t off, uint32_t val);

/* Returns the number of devices found, and a pointer to the device table. */
int                    pcie_device_count(void);
const pcie_device_t   *pcie_get_devices(void);

/* Find first device matching class/subclass/progif. Returns NULL if not found.
 * Pass 0xFF for a field to match any value. */
const pcie_device_t *pcie_find_device(uint8_t class_code, uint8_t subclass,
                                      uint8_t progif);

#endif /* PCIE_H */
```

- [ ] **Step 2: Create pcie.c**

```c
/* pcie.c — PCIe ECAM enumeration (Phase 19)
 *
 * Config space layout (ECAM):
 *   base + ((bus << 20) | (dev << 15) | (fn << 12)) = 4KB config space
 *   Offset 0x00: vendor ID (16-bit)
 *   Offset 0x02: device ID (16-bit)
 *   Offset 0x08: class code [31:24], subclass [23:16], progif [15:8], rev [7:0]
 *   Offset 0x10: BAR0 ... Offset 0x24: BAR5
 */
#include "pcie.h"
#include "acpi.h"
#include "printk.h"
#include "vmm.h"
#include "kva.h"
#include <stdint.h>

/* SAFETY: ECAM MMIO is identity-accessible in Phase 19 on QEMU q35.
 * Real hardware requires explicit vmm_map_page for the MCFG range. */
static volatile uint8_t *s_ecam_base = 0;

static pcie_device_t s_devices[PCIE_MAX_DEVICES];
static int           s_device_count = 0;

static volatile uint32_t *config_addr(uint8_t bus, uint8_t dev,
                                       uint8_t fn, uint16_t off)
{
    uint64_t offset = ((uint64_t)bus  << 20) |
                      ((uint64_t)dev  << 15) |
                      ((uint64_t)fn   << 12) |
                      (off & 0xFFC);
    return (volatile uint32_t *)(s_ecam_base + offset);
}

uint8_t pcie_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off)
{
    uint32_t val = *config_addr(bus, dev, fn, off);
    return (val >> ((off & 3) * 8)) & 0xFF;
}

uint16_t pcie_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off)
{
    uint32_t val = *config_addr(bus, dev, fn, off);
    return (val >> ((off & 2) * 8)) & 0xFFFF;
}

uint32_t pcie_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off)
{
    return *config_addr(bus, dev, fn, off & 0xFFC);
}

void pcie_write32(uint8_t bus, uint8_t dev, uint8_t fn,
                  uint16_t off, uint32_t val)
{
    *config_addr(bus, dev, fn, off & 0xFFC) = val;
}

/* Decode a BAR: returns base address, advances *bar_idx past 64-bit pairs. */
static uint64_t decode_bar(uint8_t bus, uint8_t dev, uint8_t fn,
                            int *bar_idx)
{
    uint16_t off = (uint16_t)(0x10 + (*bar_idx) * 4);
    uint32_t lo  = pcie_read32(bus, dev, fn, off);

    if ((lo & 1) == 1) {
        /* I/O BAR — skip */
        (*bar_idx)++;
        return 0;
    }

    uint8_t type = (lo >> 1) & 0x3;
    uint64_t addr = lo & ~0xFU;

    if (type == 0x2) {
        /* 64-bit BAR: next register holds upper 32 bits */
        uint32_t hi = pcie_read32(bus, dev, fn, (uint16_t)(off + 4));
        addr |= ((uint64_t)hi << 32);
        (*bar_idx)++;   /* consume the extra register */
    }

    (*bar_idx)++;
    return addr;
}

static void enumerate_function(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint16_t vendor = pcie_read16(bus, dev, fn, 0x00);
    if (vendor == 0xFFFF)
        return;     /* no device present */

    if (s_device_count >= PCIE_MAX_DEVICES)
        return;

    pcie_device_t *d = &s_devices[s_device_count++];
    d->vendor_id  = vendor;
    d->device_id  = pcie_read16(bus, dev, fn, 0x02);
    uint32_t cls  = pcie_read32(bus, dev, fn, 0x08);
    d->class_code = (cls >> 24) & 0xFF;
    d->subclass   = (cls >> 16) & 0xFF;
    d->progif     = (cls >>  8) & 0xFF;
    d->bus = bus; d->dev = dev; d->fn = fn;

    int i;
    for (i = 0; i < 6; ) {
        d->bar[i] = decode_bar(bus, dev, fn, &i);
    }

    printk("[PCIE] found %04x:%04x class=%02x at %02x:%02x.%x\n",
           d->vendor_id, d->device_id, d->class_code, bus, dev, fn);
}

void pcie_init(void)
{
    if (g_mcfg_base == 0) {
        printk("[PCIE] OK: skipped (no ECAM)\n");
        return;
    }

    /* Map the ECAM MMIO range into kernel VA.
     * QEMU q35 ECAM base is 0xB0000000 — outside the higher-half identity
     * window. Compute the number of pages needed for the bus range and
     * map them contiguously starting at a kva-allocated virtual base. */
    {
        uint32_t n_buses  = (uint32_t)(g_mcfg_end_bus - g_mcfg_start_bus + 1);
        uint32_t n_pages  = n_buses * 256;   /* 256 pages per bus (32dev×8fn×4KB) */
        uintptr_t va_base = (uintptr_t)kva_alloc_pages(n_pages);
        uint32_t  i;
        for (i = 0; i < n_pages; i++) {
            uint64_t pa = g_mcfg_base + (uint64_t)i * 4096;
            /* SAFETY: ECAM MMIO — map with present+write, no-execute.
             * PTE flags: Present(1) | Write(2) | PCD(16) | PWT(8) = 0x1B */
            vmm_map_page(vmm_get_master_pml4(), va_base + (uintptr_t)i * 4096,
                         pa, 0x1B);
        }
        s_ecam_base = (volatile uint8_t *)va_base;
    }

    uint8_t bus, dev, fn;
    for (bus = g_mcfg_start_bus; ; bus++) {
        for (dev = 0; dev < 32; dev++) {
            for (fn = 0; fn < 8; fn++) {
                uint16_t vendor = pcie_read16(bus, dev, fn, 0x00);
                if (vendor == 0xFFFF) {
                    if (fn == 0) goto next_dev;
                    continue;
                }
                enumerate_function(bus, dev, fn);
                /* Check multi-function flag */
                if (fn == 0) {
                    uint8_t hdr = pcie_read8(bus, dev, fn, 0x0E);
                    if (!(hdr & 0x80))
                        break;
                }
            }
next_dev:;
        }
        if (bus == g_mcfg_end_bus) break;
    }

    printk("[PCIE] OK: enumeration complete, %d devices\n", s_device_count);
}

int pcie_device_count(void)        { return s_device_count; }
const pcie_device_t *pcie_get_devices(void) { return s_devices; }

const pcie_device_t *pcie_find_device(uint8_t cls, uint8_t sub, uint8_t pi)
{
    int i;
    for (i = 0; i < s_device_count; i++) {
        const pcie_device_t *d = &s_devices[i];
        if ((cls == 0xFF || d->class_code == cls) &&
            (sub == 0xFF || d->subclass   == sub) &&
            (pi  == 0xFF || d->progif     == pi))
            return d;
    }
    return NULL;
}
```

- [ ] **Step 3: Build (will fail — not in Makefile yet, that's fine)**

```bash
make 2>&1 | head -10
```

Expected: either succeeds (new files not yet linked) or is unchanged. We'll add to Makefile in Task 4.

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/pcie.h kernel/arch/x86_64/pcie.c
git commit -m "arch: add PCIe ECAM enumeration (pcie.h + pcie.c)"
```

---

### Task 4: Wire ACPI + PCIe into main.c, update Makefile and boot.txt

**Files:**
- Modify: `kernel/core/main.c`
- Modify: `Makefile`
- Modify: `tests/expected/boot.txt`

- [ ] **Step 1: Update boot.txt (RED — add expected lines before implementing)**

The `make test` oracle runs on `-machine pc`. `vfs_init()` prints both `[VFS] OK:` and `[INITRD] OK:` before `acpi_init()` is called. Add the two graceful-fallback lines in `tests/expected/boot.txt` **after** `[INITRD] OK: 13 files registered` and **before** `[CAP] OK: 3 capabilities granted to init`:

```
[ACPI] OK: MADT parsed, no MCFG (legacy machine)
[PCIE] OK: skipped (no ECAM)
```

The file should now contain these lines between `[VFS]` and `[INITRD]`. Run `make test` immediately — it should **fail** (ACPI/PCIE lines not yet printed):

```bash
make test 2>&1 | tail -20
```

Expected: diff shows missing `[ACPI]` and `[PCIE]` lines.

- [ ] **Step 2: Add acpi.c and pcie.c to ARCH_SRCS in Makefile**

In `Makefile`, find the `ARCH_SRCS` block and add two lines:

```makefile
ARCH_SRCS = \
    kernel/arch/x86_64/arch.c \
    kernel/arch/x86_64/arch_exit.c \
    kernel/arch/x86_64/arch_mm.c \
    kernel/arch/x86_64/arch_vmm.c \
    kernel/arch/x86_64/acpi.c \
    kernel/arch/x86_64/pcie.c \
    kernel/arch/x86_64/serial.c \
    ...
```

Also update the CFLAGS `-I` includes to ensure the arch headers are found (they already are via `-Ikernel/arch/x86_64`).

- [ ] **Step 3: Update make run to use -machine q35**

In `Makefile`, find the `run` target and add `-machine q35`:

```makefile
run: iso
	qemu-system-x86_64 \
	    -machine q35 \
	    -cdrom $(BUILD)/aegis.iso -boot order=d \
	    -serial stdio -vga std -no-reboot -m 128M \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04
```

Note: `make test` remains on `-machine pc` (via `tests/run_tests.sh` which has its own QEMU invocation). Only `make run` changes.

- [ ] **Step 4: Wire acpi_init() and pcie_init() into main.c**

In `kernel/core/main.c`, add the headers and calls. After the `#include "vfs.h"` line, add:

```c
#include "acpi.h"
#include "pcie.h"
```

In `kernel_main()`, add the calls after `console_init()` (which registers the VFS device) and before `sched_init()`. Place them after VFS since they just print to serial:

```c
    console_init();         /* register stdout device (silent)               */
    acpi_init();            /* parse MCFG+MADT — [ACPI] OK                   */
    pcie_init();            /* enumerate PCIe devices — [PCIE] OK            */
    sched_init();
```

- [ ] **Step 5: Build**

```bash
make INIT=shell iso 2>&1 | tail -20
```

Expected: clean build. Fix any -Wunused-variable or -Wmissing-field-initializers warnings (add `(void)` casts or zero-init structs as needed).

- [ ] **Step 6: Run make test (GREEN)**

```bash
make test 2>&1 | tail -20
```

Expected: `diff` shows no differences. Exit 0.

If the diff shows unexpected output, the ACPI/PCIE init is printing something different from the expected lines. Check that:
- On `-machine pc`, `arch_get_rsdp_phys()` returns 0 (no ACPI tag)
- `acpi_init()` prints `[ACPI] OK: MADT parsed, no MCFG (legacy machine)` when rsdp_phys == 0
- `pcie_init()` prints `[PCIE] OK: skipped (no ECAM)` when g_mcfg_base == 0

- [ ] **Step 7: Commit**

```bash
git add kernel/core/main.c Makefile tests/expected/boot.txt
git commit -m "phase19: wire ACPI+PCIe into main.c, update boot.txt and Makefile"
```

---

### Task 5: Update CLAUDE.md build status

**Files:**
- Modify: `.claude/CLAUDE.md`

- [ ] **Step 1: Add Phase 19 to the Build Status table**

Find the table row for Phase 18 and add after it:

```markdown
| PCIe enumeration + ACPI (Phase 19) | ✅ Done | MCFG+MADT on q35; graceful skip on -machine pc; make test GREEN |
```

Also add a Phase 19 forward-looking constraints section:

```markdown
### Phase 19 forward-looking constraints

**ECAM MMIO is cast directly in Phase 19.** `pcie.c` casts `g_mcfg_base` (a physical address) directly to a pointer. On QEMU q35, the default ECAM base is `0xB0000000` — outside the identity-mapped `[0..4MB)` range but within the 32-bit address space. This works because Phase 7 did not remove high physical address mappings from PML4[0]. When identity map teardown is revisited, Phase 19 must replace the cast with an explicit `vmm_map_page` call for the ECAM MMIO range.

**ACPI table physical-to-virtual.** `acpi.c` casts physical table addresses directly to pointers (works within the first 4MB for QEMU). On real hardware, ACPI tables may be at addresses > 4MB — Phase 20 must add a proper `vmm_map_page` call before dereferencing large physical addresses.
```

- [ ] **Step 2: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs: update CLAUDE.md build status for Phase 19"
```

---

## Final verification

```bash
make test
```

Expected output in diff: no differences. The two new lines in boot.txt should appear in serial output when running on `-machine pc`.

```bash
make INIT=shell shell
```

Run interactively on q35. Verify the serial output includes lines like:
```
[PCIE] found 8086:1237 class=06 at 00:00.0
[PCIE] OK: enumeration complete, N devices
```
