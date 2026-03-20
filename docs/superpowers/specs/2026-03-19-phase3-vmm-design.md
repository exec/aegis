# Phase 3 — Virtual Memory Manager Design Spec

**Date:** 2026-03-19
**Status:** Approved
**Phase:** 3 of N

---

## Goal

Relocate the kernel to the higher-half virtual address `0xFFFFFFFF80000000`,
establish permanent kernel page tables (allocated from the PMM), switch `cr3`
to those tables, and expose `vmm_map_page()` / `vmm_unmap_page()` to the rest
of the kernel. After this phase, `make test` still exits 0 with one additional
line in the expected output.

---

## Scope

- **In scope:** VMA/LMA linker split, boot.asm higher-half trampoline, `-mcmodel=kernel`,
  `vmm_init()` (allocate page tables, identity window, kernel window, switch cr3),
  `vmm_map_page()` / `vmm_unmap_page()`, arch boundary for `cr3` write.
- **Out of scope:** tearing down the identity mapping (Phase 4+), user-space page
  tables, demand paging, TLB shootdown, huge-page API in `vmm_map_page` (only
  `vmm_init` uses huge pages internally), KASLR, NX enforcement (flag exists,
  enforcement deferred until NXE bit is set in EFER).

**Identity mapping kept.** The boot-time identity window `[0 .. 2MB)` is
preserved in Phase 3. `arch_mm_init()` has already consumed `mb_info` before
`vmm_init()` runs, but physical pointers may still be present elsewhere. Tearing
down the identity map is a Phase 4+ concern once the kernel has a virtual address
allocator.

---

## Memory Layout (after Phase 3)

```
0x0000000000000000
  [0 .. 2MB)       → physical [0 .. 2MB)   identity window (kept)
0x0000000000200000
  ...unmapped...
0xFFFFFFFF80000000
  [KERN_VMA .. KERN_VMA+2MB) → physical [0 .. 2MB)   kernel window
  kernel_main lives at 0xFFFFFFFF80100000
0xFFFFFFFFFFFFFFFF
```

Physical layout is unchanged: GRUB loads the kernel image at physical `0x100000`.
The linker assigns each symbol two addresses — a **load address** (physical) and
a **virtual address** (higher-half). The gap is `KERN_VMA = 0xFFFFFFFF80000000`.

---

## Files

| File | Action | Responsibility |
|------|--------|----------------|
| `tools/linker.ld` | Modify | VMA/LMA split; `.multiboot` + `.text.boot` at physical; rest at `KERN_VMA + phys_offset` |
| `kernel/arch/x86_64/boot.asm` | Modify | Add `pdpt_hi` + `pd_hi`; build higher-half mapping; physical far-jump; absolute jump to higher-half before `call kernel_main` |
| `kernel/arch/x86_64/arch.h` | Modify | Add `ARCH_KERNEL_VIRT_BASE`; declare `arch_vmm_load_pml4()` and `arch_vmm_invlpg()` |
| `kernel/arch/x86_64/arch_vmm.c` | Create | `arch_vmm_load_pml4()` and `arch_vmm_invlpg()` — only place `cr3`/`invlpg` are used |
| `kernel/mm/vmm.h` | Create | Public VMM interface |
| `kernel/mm/vmm.c` | Create | Page table allocator (arch-agnostic); calls `arch_vmm_load_pml4` |
| `kernel/mm/pmm.c` | Modify | Fix `_kernel_end` physical computation (now a virtual address) |
| `kernel/core/main.c` | Modify | Add `vmm_init()` call between `pmm_init()` and `cap_init()` |
| `Makefile` | Modify | Add `-mcmodel=kernel`; add `arch_vmm.c` to `ARCH_SRCS`; add `vmm.c` to `MM_SRCS` |
| `tests/expected/boot.txt` | Modify | Add `[VMM] OK` line (RED before implementation) |
| `.claude/CLAUDE.md` | Modify | Mark VMM in-progress; document VMA/LMA split and identity-mapping decision |

---

## Section 1: Linker Script

```ld
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHYS_BASE = 0x100000;
KERN_VMA  = 0xFFFFFFFF80000000;

SECTIONS
{
    . = PHYS_BASE;

    /* Boot trampoline: VMA = LMA = physical.
     * 32-bit setup code and early 64-bit trampoline run here before
     * jumping to the higher-half virtual address. */
    .multiboot : { KEEP(*(.multiboot)) }
    .text.boot  : { *(.text.boot) }

    /* Switch to higher-half virtual addresses.
     * AT(...) sets the LMA (load/physical address) for each section. */
    . += KERN_VMA;

    .text   : AT(ADDR(.text)   - KERN_VMA) { *(.text .text.*) }
    .rodata : AT(ADDR(.rodata) - KERN_VMA) { *(.rodata .rodata.*) }
    .data   : AT(ADDR(.data)   - KERN_VMA) { *(.data .data.*) }

    .bss : AT(ADDR(.bss) - KERN_VMA) {
        *(COMMON)
        *(.bss .bss.*)
    }
    _kernel_end = .;   /* higher-half virtual address */
}
```

`_kernel_end` is now a higher-half virtual address. `pmm.c` computes the
physical kernel end as `_kernel_end - ARCH_KERNEL_VIRT_BASE`.

---

## Section 2: boot.asm Changes

### 2a. Section layout and physical address macro

The existing boot code has two sections: `section .multiboot` and `section .data`
(for the GDT) and `section .text` (for `_start`) and `section .bss`.

After the VMA/LMA split, `section .data` and `section .text` get higher-half VMAs.
This breaks the GDT: `lgdt [gdt64_ptr]` runs in 32-bit mode before paging, so
`gdt64_ptr` must be at its physical address, and the `dd gdt64` pointer inside it
must also be physical. Both are wrong if they live in `.data` post-split.

**Fix:** move `gdt64:` and `gdt64_ptr:` from `section .data` into `section .text.boot`.
The GDT is tiny (24 bytes) and boot-only; living in the trampoline code section is fine.
All code and data that runs before the higher-half jump must be in `section .text.boot`.

Similarly, all `.bss` labels (page tables, stack) get higher-half VMAs. In 32-bit
mode their physical addresses are needed. NASM evaluates constant expressions at
assemble time — the result fits in 32 bits:

```nasm
KERN_VMA equ 0xFFFFFFFF80000000

; Physical address of a label (result fits in 32 bits):
mov eax, (pml4_table - KERN_VMA)
```

### 2b. Two new page tables

```nasm
section .bss
align 4096
pml4_table: resb 4096
align 4096
pdpt_lo:    resb 4096   ; identity (was pdpt_table)
align 4096
pdpt_hi:    resb 4096   ; higher-half (new)
align 4096
pd_lo:      resb 4096   ; identity (was pd_table)
align 4096
pd_hi:      resb 4096   ; higher-half (new)
```

Page table wiring (all entries use physical addresses of the target table):

```
pml4[0]      → pdpt_lo    (present | writable)
pml4[511]    → pdpt_hi    (present | writable)
pdpt_lo[0]   → pd_lo      (present | writable)
pdpt_hi[510] → pd_hi      (present | writable)   ← index 510 = bits 38:30 of KERN_VMA
pd_lo[0]     → 0x0        (present | writable | PS)   2MB identity huge page
pd_hi[0]     → 0x0        (present | writable | PS)   2MB kernel window huge page
```

PDPT index 510: bits 38:30 of `0xFFFFFFFF80000000` = `0b111111110` = 510.

### 2c. Jump sequence

The 32-bit code up through `CR0.PG` enable stays in `section .text.boot` and
uses `(label - KERN_VMA)` wherever a physical address is needed.

After paging is enabled, the far jump must use a **physical** 32-bit offset (the
CPU is still in 32-bit protected mode at this point, so the far-jump offset field
is 32-bit). `long_mode_phys` is in `.text.boot` so its VMA equals its LMA.

Once in 64-bit mode we do an absolute jump to the higher-half entry point, which
is in `section .text`. Because this label is referenced across sections it must be
a **global** (no leading dot), not a local label:

```nasm
section .text.boot
bits 32

; ... (32-bit setup, page table wiring, CR4/EFER/CR0) ...

    ; Far jump: activates 64-bit mode, lands at physical address (identity-mapped).
    jmp 0x08:long_mode_phys

bits 64
long_mode_phys:
    ; Executing at physical address (~0x100000 range), identity-mapped. Safe.
    ; Jump to higher-half virtual address (label is in section .text).
    mov rax, long_mode_high
    jmp rax


section .text
bits 64
global long_mode_high
long_mode_high:
    ; Executing at 0xFFFFFFFF80xxxxxx — higher half is live.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax
    mov rsp, boot_stack_top   ; higher-half virtual address (correct)
    extern kernel_main
    call kernel_main           ; higher-half virtual address (correct)
.hang:
    hlt
    jmp .hang
```

Key constraints:
- `long_mode_phys` is in `section .text.boot` (VMA = LMA = physical) — far-jump offset is valid in 32-bit protected mode
- `long_mode_high` is in `section .text` (VMA = higher-half) — global, not a local label (`.long_mode_high` would be scoped to the current section only and unreachable from `.text.boot`)
- Segment register loads and stack setup happen in `long_mode_high` (higher-half), not `long_mode_phys`
- The existing `extern kernel_main` declaration moves from `.text.boot` to `section .text` (near the call site)

---

## Section 3: arch.h Additions

```c
/* Virtual base address of the kernel image.
 * pmm.c uses this to convert _kernel_end (virtual) to a physical address. */
#define ARCH_KERNEL_VIRT_BASE 0xFFFFFFFF80000000UL

/* Load the physical address of a PML4 into CR3.
 * Implemented in arch_vmm.c — the only place in the codebase that writes CR3.
 * Flushes the TLB entirely (CR3 reload). */
void arch_vmm_load_pml4(uint64_t phys);

/* Invalidate the TLB entry for a single virtual address.
 * Implemented in arch_vmm.c — the only place in the codebase that uses invlpg.
 * Must be called after clearing a PTE in vmm_unmap_page(). */
void arch_vmm_invlpg(uint64_t virt);
```

---

## Section 4: arch_vmm.c

```c
#include "arch.h"
#include <stdint.h>

void arch_vmm_load_pml4(uint64_t phys)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(phys) : "memory");
}

void arch_vmm_invlpg(uint64_t virt)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}
```

Two functions. All `cr3` writes and `invlpg` instructions in the kernel go through
this file. No other file touches `cr3` or issues `invlpg` directly.

---

## Section 5: VMM Public Interface (vmm.h)

```c
#ifndef VMM_H
#define VMM_H

#include <stdint.h>

/* Page-table entry flag bits (x86-64 PTE format). */
#define VMM_FLAG_PRESENT   (1UL << 0)
#define VMM_FLAG_WRITABLE  (1UL << 1)
#define VMM_FLAG_USER      (1UL << 2)
#define VMM_FLAG_NX        (1UL << 63)

/* Initialise the virtual memory manager.
 * Requires pmm_init() to have been called first.
 * Allocates permanent kernel page tables from the PMM, switches cr3,
 * and prints [VMM] OK. Panics on failure. */
void vmm_init(void);

/* Map one 4KB page: virt → phys with given flags.
 * Allocates intermediate page tables from PMM as needed.
 * Panics if virt or phys is not PAGE_SIZE-aligned, or if virt is already mapped. */
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/* Unmap one 4KB page. Panics if not mapped.
 * Does not free intermediate tables (deferred to a future reclaim pass). */
void vmm_unmap_page(uint64_t virt);

#endif /* VMM_H */
```

---

## Section 6: vmm.c Structure

```c
#include "vmm.h"
#include "pmm.h"
#include "arch.h"     /* arch_vmm_load_pml4, ARCH_KERNEL_VIRT_BASE */
#include "printk.h"
#include <stdint.h>
#include <stddef.h>

#define PML4_IDX(v) (((v) >> 39) & 0x1FF)
#define PDPT_IDX(v) (((v) >> 30) & 0x1FF)
#define PD_IDX(v)   (((v) >> 21) & 0x1FF)
#define PT_IDX(v)   (((v) >> 12) & 0x1FF)

#define PTE_PRESENT  VMM_FLAG_PRESENT
#define PTE_WRITABLE VMM_FLAG_WRITABLE
#define PTE_PS       (1UL << 7)   /* huge page (PD level) */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000UL
```

### Page zeroing

`pmm_alloc_page()` returns unzeroed pages. Unzeroed page table pages contain
garbage that the CPU may interpret as present PTEs — a security hazard. Every
page allocated for a page table structure must be zeroed before any entry is written.

Use a static helper (internal to `vmm.c`):

```c
static void zero_page(uint64_t phys)
{
    /* Safe while the identity window [0..2MB) is active: physical == virtual.
     * CONSTRAINT: phys must be < 2MB. All PMM allocations in Phase 3 satisfy
     * this because the kernel + bitmap fit well within 2MB on a 128MB QEMU
     * machine. If the kernel image ever grows past 2MB, or if vmm_map_page()
     * is called with a deep mapping that triggers a PMM allocation above 2MB,
     * this will write to an unmapped address and fault. Phase 4, which tears
     * down or expands the identity map, must replace this with a mapped-window
     * allocator. */
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)phys;
    for (int i = 0; i < 512; i++)
        p[i] = 0;
}
```

This is valid in Phase 3 because the identity window keeps `phys == virt` for all
PMM-allocated pages that are below 2MB. The identity window is present in the new
PML4 built by `vmm_init()` (step 2), so `zero_page()` is safe in `vmm_map_page()`
calls after the cr3 switch — as long as the PMM does not return a page at or above
2MB. Phase 4 must revisit this when mappings grow or identity is torn down.

### vmm_init() sequence

```
1. pml4_phys = pmm_alloc_page(); zero_page(pml4_phys).

2. Map identity window [0 .. 2MB):
   - pdpt_phys = pmm_alloc_page(); zero_page(pdpt_phys)
   - pd_phys   = pmm_alloc_page(); zero_page(pd_phys)
   - pml4[0]    = pdpt_phys | PRESENT | WRITABLE
   - pdpt[0]    = pd_phys   | PRESENT | WRITABLE
   - pd[0]      = 0x0       | PRESENT | WRITABLE | PS   (2MB huge page)

3. Map kernel window [KERN_VMA .. KERN_VMA+2MB):
   - pdpt_hi_phys = pmm_alloc_page(); zero_page(pdpt_hi_phys)
   - pd_hi_phys   = pmm_alloc_page(); zero_page(pd_hi_phys)
   - pml4[511]    = pdpt_hi_phys | PRESENT | WRITABLE
   - pdpt_hi[510] = pd_hi_phys   | PRESENT | WRITABLE
   - pd_hi[0]     = 0x0          | PRESENT | WRITABLE | PS

4. arch_vmm_load_pml4(pml4_phys);

5. printk("[VMM] OK: kernel mapped to 0xFFFFFFFF80000000\n");
```

Step 3 uses the same 2MB huge-page approach as the identity window —
the kernel fits within the first 2MB of physical space, so one PD entry suffices.
`vmm_map_page` (4KB granularity) is used by callers for arbitrary future mappings.

### vmm_map_page() walk

Walk PML4 → PDPT → PD → PT. At each level: if entry not present, allocate a
new page from PMM, call `zero_page()`, install it. At PT level: if entry already
present, print `[VMM] FAIL: double-map at <addr>` then `for (;;) {}`. Otherwise
set the PT entry to `(phys & PTE_ADDR_MASK) | flags | PRESENT`.

### vmm_unmap_page()

Walk to PT. If entry not present, print `[VMM] FAIL: unmap of unmapped addr` then
`for (;;) {}`. Clear the entry. Call `arch_vmm_invlpg(virt)` to flush the TLB
entry for this address.

**Panic pattern** (used by vmm_map_page, vmm_unmap_page, and vmm_init on OOM):
```c
printk("[VMM] FAIL: <reason>\n");
for (;;) {}
```
This is consistent with `pmm.c`'s error handling and ensures the test harness
captures the failure message before the halt.

---

## Section 7: pmm.c Fix

`_kernel_end` is now a higher-half virtual address. The kernel reservation in
`pmm_init()` changes from:

```c
/* OLD (Phase 2) */
pmm_reserve_region(ARCH_KERNEL_PHYS_BASE,
    (uint64_t)(uintptr_t)_kernel_end - ARCH_KERNEL_PHYS_BASE);
```

to:

```c
/* NEW (Phase 3) */
pmm_reserve_region(ARCH_KERNEL_PHYS_BASE,
    (uint64_t)(uintptr_t)_kernel_end - ARCH_KERNEL_VIRT_BASE - ARCH_KERNEL_PHYS_BASE);
```

`(uintptr_t)_kernel_end - ARCH_KERNEL_VIRT_BASE` = physical end of kernel image.
Subtract `ARCH_KERNEL_PHYS_BASE` to get the length from the kernel load address.

---

## Section 8: main.c Boot Sequence

```c
arch_init();           /* serial + VGA                              */
arch_mm_init(mb_info); /* parse multiboot2 memory map              */
                       /* NOTE: mb_info is physical; arch_mm_init  */
                       /* consumes it before vmm_init runs.         */
pmm_init();            /* bitmap allocator — [PMM] OK              */
vmm_init();            /* page tables, cr3 switch — [VMM] OK       */
cap_init();            /* capability stub — [CAP] OK               */
printk("[AEGIS] System halted.\n");
arch_debug_exit(0x01);
```

---

## Section 9: Test Harness

### tests/expected/boot.txt (after Phase 3)

```
[SERIAL] OK: COM1 initialized at 115200 baud
[VGA] OK: text mode 80x25
[PMM] OK: 127MB usable across 2 regions
[VMM] OK: kernel mapped to 0xFFFFFFFF80000000
[CAP] OK: capability subsystem reserved
[AEGIS] System halted.
```

**Note on the PMM line:** The numbers `127MB` and `2 regions` match the Phase 2
baseline from QEMU `-m 128M` with SeaBIOS. These values come from the multiboot2
memory map and are not affected by the VMM changes. However, if the first GREEN
run shows different numbers (e.g. because a GRUB or SeaBIOS update changed the
reported map), update `boot.txt` to match the actual output and document the
deviation. Do not hard-code numbers you haven't verified from a live boot.

### TDD order

1. Add VMM line to `boot.txt` → `make test` fails (RED)
2. Implement all changes → `make test` passes (GREEN)
3. Review → refactor if needed

---

## Section 10: Makefile Changes

```makefile
ARCH_SRCS = \
    kernel/arch/x86_64/arch.c \
    kernel/arch/x86_64/arch_exit.c \
    kernel/arch/x86_64/arch_mm.c \
    kernel/arch/x86_64/arch_vmm.c \
    kernel/arch/x86_64/serial.c \
    kernel/arch/x86_64/vga.c

MM_SRCS = \
    kernel/mm/pmm.c \
    kernel/mm/vmm.c
```

Add `-mcmodel=kernel` to CFLAGS. This tells GCC that all code and data reside
in the top 2GB of the virtual address space (`0xFFFFFFFF00000000` and above),
allowing it to generate more efficient RIP-relative addressing without
relocations that assume a low-address kernel.

---

## Decisions Log

| Question | Decision | Reason |
|----------|----------|--------|
| Higher-half relocation now vs. deferred | Now (Phase 3) | Doing it later means updating more code; every subsequent phase assumes KERN_VMA |
| Identity mapping kept or torn down | Kept in Phase 3 | `arch_mm_init` consumed `mb_info` but other physical refs may exist; tear-down deferred to Phase 4+ |
| Huge pages for kernel window | Yes (2MB, PD level) | Kernel fits in first 2MB; avoids allocating PT pages for the kernel window itself |
| `cr3` write location | `arch_vmm.c` only | Keeps `kernel/mm/` arch-agnostic; same pattern as arch boundary for PMM |
| `invlpg` location | `arch_vmm.c` (`arch_vmm_invlpg`) | Same reason — arch-specific instruction |
| `vmm_map_page` huge-page support | Not in public API | YAGNI; `vmm_init` handles huge pages internally; callers use 4KB API |
| `zero_page()` validity post-cr3 | Relies on identity window extent (< 2MB) | All Phase 3 PMM allocations land below 2MB on a 128MB machine; Phase 4 must replace with a mapped-window allocator if identity is torn down or kernel grows past 2MB |
| Freeing intermediate tables on unmap | Deferred | Requires a reclaim pass; out of scope for Phase 3 |
| NX enforcement | Flag defined, not enforced | NXE bit in EFER not yet set; add in the phase that sets up proper segment/privilege separation |
