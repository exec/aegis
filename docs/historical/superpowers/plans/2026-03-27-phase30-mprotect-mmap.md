# Phase 30: mprotect + mmap Improvements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the mprotect stub with real page-permission changes and add a per-process mmap VA freelist so munmap'd regions are reused.

**Architecture:** Two independent features in the same subsystem: (1) `vmm_set_user_prot()` + real `sys_mprotect` that walks page tables and updates PTE flags in-place with TLB flush; (2) a 64-slot static freelist in `aegis_process_t` with coalescing insert and best-fit alloc, wired into `sys_munmap`/`sys_mmap`.

**Tech Stack:** C kernel code (x86-64), musl libc user binaries, QEMU test harness

---

### Task 1: Add `vmm_set_user_prot()` to VMM

**Files:**
- Modify: `kernel/mm/vmm.h` — add declaration
- Modify: `kernel/mm/vmm.c` — add implementation

**Context:** The existing `vmm_unmap_user_page()` at `vmm.c:537` shows the walk-overwrite pattern: `vmm_window_map(pml4_phys)` → read PML4e → `vmm_window_map(ARCH_PTE_ADDR(pml4e))` → read PDPTe → etc. The new function follows the same pattern but modifies the leaf PTE flags in-place instead of clearing it. `ARCH_PTE_ADDR(pte)` extracts the physical address from a PTE (bits [51:12]). `arch_pte_from_flags()` converts VMM_FLAG_* to hardware PTE bits (identity on x86-64). `arch_vmm_invlpg(virt)` flushes a single TLB entry.

- [ ] **Step 1: Add declaration to vmm.h**

In `kernel/mm/vmm.h`, after the `vmm_unmap_user_page` declaration (line 100), add:

```c
/* vmm_set_user_prot — change PTE flags for a single user page in pml4_phys.
 * Walks the 4-level page table to the leaf PTE, preserves the physical address,
 * and overwrites the flag bits. Issues invlpg for virt.
 * flags=0 clears PRESENT (PROT_NONE). Returns 0 on success, -1 if the page
 * is not mapped (caller should skip silently, matching Linux mprotect). */
int vmm_set_user_prot(uint64_t pml4_phys, uint64_t virt, uint64_t flags);
```

- [ ] **Step 2: Implement vmm_set_user_prot in vmm.c**

In `kernel/mm/vmm.c`, after `vmm_unmap_user_page` (after line 564), add:

```c
int
vmm_set_user_prot(uint64_t pml4_phys, uint64_t virt, uint64_t flags)
{
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    /* Walk-overwrite pattern. Return -1 if any level is absent. */
    uint64_t *pml4  = vmm_window_map(pml4_phys);
    uint64_t  pml4e = pml4[pml4_idx];
    if (!(pml4e & VMM_FLAG_PRESENT)) { vmm_window_unmap(); return -1; }

    uint64_t *pdpt  = vmm_window_map(ARCH_PTE_ADDR(pml4e));
    uint64_t  pdpte = pdpt[pdpt_idx];
    if (!(pdpte & VMM_FLAG_PRESENT)) { vmm_window_unmap(); return -1; }

    uint64_t *pd  = vmm_window_map(ARCH_PTE_ADDR(pdpte));
    uint64_t  pde = pd[pd_idx];
    if (!(pde & VMM_FLAG_PRESENT) || (pde & PTE_PS)) {
        vmm_window_unmap(); return -1;
    }

    uint64_t *pt  = vmm_window_map(ARCH_PTE_ADDR(pde));
    uint64_t  old = pt[pt_idx];

    /* If page was never mapped (no physical address), skip. */
    if (!(old & VMM_FLAG_PRESENT) && flags == 0) {
        vmm_window_unmap();
        return 0;  /* PROT_NONE on unmapped page is a no-op */
    }
    if (!(old & VMM_FLAG_PRESENT)) {
        vmm_window_unmap();
        return -1;  /* can't set real prot on unmapped page */
    }

    /* Preserve physical address, replace flags. */
    uint64_t phys = ARCH_PTE_ADDR(old);
    if (flags == 0) {
        /* PROT_NONE: store phys with no PRESENT bit.
         * CPU ignores the PTE but we keep the address for munmap. */
        pt[pt_idx] = phys;
    } else {
        pt[pt_idx] = phys | arch_pte_from_flags(flags);
    }
    vmm_window_unmap();
    arch_vmm_invlpg(virt);
    return 0;
}
```

- [ ] **Step 3: Build and verify compilation**

Run: `make` (on the remote build machine)
Expected: Clean compile, no warnings.

- [ ] **Step 4: Run `make test` to verify no regressions**

Run: `make test`
Expected: Boot oracle PASS (mprotect is still a stub in syscall layer; VMM function is just added, not called yet).

- [ ] **Step 5: Commit**

```bash
git add kernel/mm/vmm.h kernel/mm/vmm.c
git commit -m "feat: add vmm_set_user_prot() — modify PTE flags in-place

Walks 4-level page table via window allocator, preserves physical
address, overwrites flag bits, issues invlpg. For PROT_NONE, clears
PRESENT but keeps phys address in PTE for munmap to find."
```

---

### Task 2: Implement real `sys_mprotect`

**Files:**
- Modify: `kernel/syscall/sys_memory.c:174-183` — replace mprotect stub
- Modify: `kernel/syscall/sys_impl.h` — no change needed (declaration already exists)

**Context:** The stub is at `sys_memory.c:178-183`. Replace it with validation + a per-page loop calling `vmm_set_user_prot()`. The PROT constants are already defined in `sys_impl.h`: `PROT_READ=1`, `PROT_WRITE=2`. Add `PROT_EXEC=4` if not present. `USER_ADDR_MAX` is `0x00007FFFFFFFFFFFULL` from `arch.h`.

- [ ] **Step 1: Check if PROT_EXEC is defined**

Look in `kernel/syscall/sys_impl.h` for PROT_EXEC. If not defined, add it after PROT_WRITE.

Add to `kernel/syscall/sys_impl.h` after the existing PROT defines:

```c
#define PROT_EXEC       0x04
```

- [ ] **Step 2: Replace the mprotect stub**

Replace `sys_memory.c` lines 173-183 (the comment and stub function) with:

```c
/*
 * sys_mprotect — syscall 10
 *
 * arg1 = addr (must be page-aligned)
 * arg2 = len (rounded up to page boundary)
 * arg3 = prot (PROT_NONE, PROT_READ, PROT_WRITE, PROT_EXEC combinations)
 *
 * Changes page permissions for [addr, addr+len). Unmapped pages are
 * silently skipped (matching Linux). W^X: NX is set by default; only
 * an explicit PROT_EXEC clears NX.
 */
uint64_t
sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot)
{
    if (addr & 0xFFFUL)
        return (uint64_t)-(int64_t)22;   /* -EINVAL: not page-aligned */

    uint64_t rlen = (len + 4095UL) & ~4095UL;
    if (rlen == 0)
        return 0;  /* zero-length is a no-op */

    if (addr + rlen > USER_ADDR_MAX || addr + rlen < addr)
        return (uint64_t)-(int64_t)22;   /* -EINVAL: exceeds user space */

    /* Map PROT_* to VMM_FLAG_*.
     * x86 can't do write-only or exec-only; implicitly add READ. */
    uint64_t flags;
    if (prot == 0) {
        flags = 0;  /* PROT_NONE: clear PRESENT */
    } else {
        flags = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_NX;
        if (prot & PROT_WRITE)
            flags |= VMM_FLAG_WRITABLE;
        if (prot & PROT_EXEC)
            flags &= ~VMM_FLAG_NX;  /* clear NX for executable pages */
    }

    aegis_process_t *proc = (aegis_process_t *)sched_current();
    uint64_t va;
    for (va = addr; va < addr + rlen; va += 4096UL)
        vmm_set_user_prot(proc->pml4_phys, va, flags);

    return 0;
}
```

- [ ] **Step 3: Relax the prot validation in sys_mmap**

In `sys_mmap` (line 98), the current check rejects any prot with PROT_EXEC or PROT_NONE bits set:
```c
if ((arg3 & ~(uint64_t)(PROT_READ | PROT_WRITE)) != 0)
```

Replace with a permissive check that allows any valid combination:
```c
if (arg3 & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC))
```

This allows PROT_NONE (arg3=0) through as well (0 & ~7 = 0, passes).

- [ ] **Step 4: Add NX to sys_mmap default page flags**

In `sys_mmap`, change the `vmm_map_user_page` call (line 134-135) from:
```c
vmm_map_user_page(proc->pml4_phys, va, phys,
                  VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE);
```
to:
```c
vmm_map_user_page(proc->pml4_phys, va, phys,
                  VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE | VMM_FLAG_NX);
```

This ensures all mmap'd pages are non-executable by default (W^X baseline). musl calls mprotect(PROT_READ|PROT_EXEC) for executable mappings.

- [ ] **Step 5: Build and test**

Run: `make test`
Expected: Boot oracle PASS. The mprotect is now real but the behavior is compatible — musl's `mprotect(PROT_READ|PROT_WRITE)` after `mmap` still works.

- [ ] **Step 6: Run thread test**

Run: `python3 tests/test_threads.py`
Expected: PASS — musl's pthread_create calls mmap(PROT_NONE) then mprotect(PROT_READ|PROT_WRITE) for the guard page pattern. Our real mprotect now enforces this.

- [ ] **Step 7: Commit**

```bash
git add kernel/syscall/sys_memory.c kernel/syscall/sys_impl.h
git commit -m "feat: real sys_mprotect — page permission changes + W^X

Replace stub with per-page vmm_set_user_prot loop. PROT_NONE clears
PRESENT; NX set by default on all mmap'd pages; PROT_EXEC clears NX.
sys_mmap now allows PROT_NONE/PROT_EXEC and sets NX on all new pages."
```

---

### Task 3: Add mmap freelist to `aegis_process_t`

**Files:**
- Modify: `kernel/proc/proc.h` — add freelist struct and fields

**Context:** The PCB is allocated as `kva_alloc_pages(1)` = 4096 bytes. Current struct is ~2736 bytes. A 64-slot freelist adds 64×16 + 4 = 1028 bytes, fitting within the remaining ~1360 bytes.

- [ ] **Step 1: Add freelist fields to proc.h**

In `kernel/proc/proc.h`, add the freelist type before the `aegis_process_t` typedef (before line 23), and add the fields inside the struct after `mmap_base` (after line 30):

Before the struct definition, add:

```c
#define MMAP_FREE_MAX 64

typedef struct {
    uint64_t base;
    uint64_t len;
} mmap_free_t;
```

Inside `aegis_process_t`, after the `mmap_base` field (line 30), add:

```c
    mmap_free_t   mmap_free[MMAP_FREE_MAX]; /* VA freelist for munmap→mmap reuse */
    uint32_t      mmap_free_count;          /* number of entries in mmap_free[]  */
```

- [ ] **Step 2: Build and verify no regressions**

Run: `make test`
Expected: PASS — the new fields are zero-initialized by `kva_alloc_pages` (which calls `vmm_zero_page`), so `mmap_free_count = 0` by default.

- [ ] **Step 3: Commit**

```bash
git add kernel/proc/proc.h
git commit -m "feat: add mmap_free[] freelist to aegis_process_t

64-slot (base, len) array for munmap→mmap VA reuse. Zero-initialized
by kva_alloc_pages. 1028 bytes fits within the 1-page PCB allocation."
```

---

### Task 4: Implement freelist operations + wire into sys_munmap and sys_mmap

**Files:**
- Modify: `kernel/syscall/sys_memory.c` — add freelist functions, update sys_munmap and sys_mmap

**Context:** `sys_munmap` is at line 154. After the physical-page-free loop, call `mmap_free_insert`. `sys_mmap` allocates VA from `proc->mmap_base` at line 107. Before that, try `mmap_free_alloc` first. Both functions are static helpers in `sys_memory.c`.

- [ ] **Step 1: Add freelist insert with coalescing**

In `kernel/syscall/sys_memory.c`, before `sys_mmap` (before line 78), add:

```c
/* ── mmap VA freelist helpers ─────────────────────────────────────────── */

/* Insert a freed VA region into the process freelist. Coalesces with
 * adjacent entries. Drops the VA silently if the freelist is full. */
static void
mmap_free_insert(aegis_process_t *proc, uint64_t base, uint64_t len)
{
    uint32_t i;

    /* Try to merge with an existing entry. */
    for (i = 0; i < proc->mmap_free_count; i++) {
        mmap_free_t *e = &proc->mmap_free[i];
        if (e->base + e->len == base) {
            /* Extends existing region at the end. */
            e->len += len;
            /* Check for double-coalesce: merged entry may now be contiguous
             * with another entry on the right side. */
            uint32_t j;
            for (j = 0; j < proc->mmap_free_count; j++) {
                if (j == i) continue;
                if (e->base + e->len == proc->mmap_free[j].base) {
                    e->len += proc->mmap_free[j].len;
                    /* Remove entry j: swap with last. */
                    proc->mmap_free[j] = proc->mmap_free[--proc->mmap_free_count];
                    break;
                }
            }
            return;
        }
        if (base + len == e->base) {
            /* Extends existing region at the front. */
            e->base = base;
            e->len += len;
            /* Double-coalesce: check if another entry ends where we now start. */
            uint32_t j;
            for (j = 0; j < proc->mmap_free_count; j++) {
                if (j == i) continue;
                if (proc->mmap_free[j].base + proc->mmap_free[j].len == e->base) {
                    proc->mmap_free[j].len += e->len;
                    /* Remove entry i: swap with last. */
                    proc->mmap_free[i] = proc->mmap_free[--proc->mmap_free_count];
                    break;
                }
            }
            return;
        }
    }

    /* No merge — append if space available. */
    if (proc->mmap_free_count < MMAP_FREE_MAX) {
        proc->mmap_free[proc->mmap_free_count].base = base;
        proc->mmap_free[proc->mmap_free_count].len  = len;
        proc->mmap_free_count++;
    }
    /* else: freelist full — drop VA (graceful degradation) */
}

/* Allocate VA from the freelist (best-fit). Returns base VA, or 0 if
 * no suitable region is found (caller falls back to bump allocator). */
static uint64_t
mmap_free_alloc(aegis_process_t *proc, uint64_t len)
{
    uint32_t best = (uint32_t)-1;
    uint64_t best_len = (uint64_t)-1;
    uint32_t i;

    for (i = 0; i < proc->mmap_free_count; i++) {
        if (proc->mmap_free[i].len >= len && proc->mmap_free[i].len < best_len) {
            best = i;
            best_len = proc->mmap_free[i].len;
            if (best_len == len) break;  /* exact fit — can't do better */
        }
    }

    if (best == (uint32_t)-1)
        return 0;  /* nothing fits */

    uint64_t base = proc->mmap_free[best].base;
    if (proc->mmap_free[best].len == len) {
        /* Exact fit: remove entry (swap with last). */
        proc->mmap_free[best] = proc->mmap_free[--proc->mmap_free_count];
    } else {
        /* Larger: carve from front. */
        proc->mmap_free[best].base += len;
        proc->mmap_free[best].len  -= len;
    }
    return base;
}
```

- [ ] **Step 2: Wire freelist insert into sys_munmap**

In `sys_munmap`, after the physical-page-free loop (after line 169, before `return 0;`), add:

```c
    /* Return VA range to freelist for reuse by future mmap calls. */
    mmap_free_insert(proc, arg1, len);
```

- [ ] **Step 3: Wire freelist alloc into sys_mmap**

In `sys_mmap`, replace the block from line 107 (`uint64_t base = proc->mmap_base;`) through line 113 (the overflow check) with:

```c
    /* Try freelist first; fall back to bump allocator. */
    uint64_t base = mmap_free_alloc(proc, len);
    if (base == 0) {
        base = proc->mmap_base;
        if (base + len > USER_ADDR_MAX || base + len < base)
            return (uint64_t)-(int64_t)12;  /* -ENOMEM */
    }
```

Then change the `proc->mmap_base` advancement at the end (line 138-140) to only advance when the bump allocator was used:

Replace:
```c
    if (proc->mmap_base + len > USER_ADDR_MAX || proc->mmap_base + len < proc->mmap_base)
        return (uint64_t)-(int64_t)12;  /* -ENOMEM */
    proc->mmap_base += len;
```
with:
```c
    /* Advance bump allocator only if we used it (not freelist). */
    if (base >= proc->mmap_base)
        proc->mmap_base = base + len;
```

(If `base` came from the freelist, it will be below `mmap_base` — don't advance.)

- [ ] **Step 4: Handle PROT_NONE in vmm_phys_of_user for munmap**

Currently `vmm_phys_of_user` returns 0 if PRESENT is not set (line 533). But after `mprotect(PROT_NONE)`, the PTE has the physical address but no PRESENT bit. We need `sys_munmap` to still find and free the physical page.

In `kernel/mm/vmm.c`, change `vmm_phys_of_user` (line 530-533). Replace:

```c
    uint64_t *pt  = vmm_window_map(ARCH_PTE_ADDR(pde));
    uint64_t  pte = pt[pt_idx];
    vmm_window_unmap();

    return (pte & VMM_FLAG_PRESENT) ? ARCH_PTE_ADDR(pte) : 0;
```

with:

```c
    uint64_t *pt  = vmm_window_map(ARCH_PTE_ADDR(pde));
    uint64_t  pte = pt[pt_idx];
    vmm_window_unmap();

    /* Return phys addr if PRESENT, or if phys bits are set (PROT_NONE page:
     * PRESENT cleared by mprotect but physical address preserved in PTE). */
    uint64_t phys = ARCH_PTE_ADDR(pte);
    return phys;
```

Similarly, update `vmm_unmap_user_page` (line 560) to clear non-PRESENT PTEs too. Replace:

```c
    uint64_t *pt  = vmm_window_map(ARCH_PTE_ADDR(pde));
    if (pt[pt_idx] & VMM_FLAG_PRESENT)
        pt[pt_idx] = 0;
```

with:

```c
    uint64_t *pt  = vmm_window_map(ARCH_PTE_ADDR(pde));
    if (pt[pt_idx] != 0)  /* clear both PRESENT and PROT_NONE (phys-only) PTEs */
        pt[pt_idx] = 0;
```

- [ ] **Step 5: Also handle vmm_map_user_page double-map check for PROT_NONE pages**

In `vmm_map_user_page` (vmm.c ~line 430), the double-map check currently panics if PRESENT is set. But after mprotect(PROT_NONE) + munmap, the PTE is 0 (cleared by unmap). For freelist reuse, the PTE must be 0 before re-mapping. The existing code already checks `pt[pt_idx] & VMM_FLAG_PRESENT` — but a PROT_NONE PTE has phys bits set without PRESENT. Change the panic check to catch both cases:

Replace:
```c
    if (pt[pt_idx] & VMM_FLAG_PRESENT) {
```
with:
```c
    if (pt[pt_idx] != 0) {
```

Wait — this would break freelist reuse because `vmm_unmap_user_page` already clears the PTE to 0. So after munmap → freelist → mmap, the PTE IS 0. The double-map check `pt[pt_idx] != 0` would only fire if we try to map over an existing mapping without unmapping first, which is a real bug. This is actually a better check. Make this change.

- [ ] **Step 6: Build and test**

Run: `make test`
Expected: Boot oracle PASS.

Run: `python3 tests/test_threads.py`
Expected: PASS — thread stacks are now recycled via the freelist.

- [ ] **Step 7: Commit**

```bash
git add kernel/syscall/sys_memory.c kernel/mm/vmm.c
git commit -m "feat: mmap VA freelist — munmap returns VA for reuse

Best-fit alloc with coalescing insert. sys_mmap tries freelist before
bump allocator. vmm_phys_of_user now finds PROT_NONE pages (phys addr
preserved in PTE without PRESENT bit). vmm_unmap_user_page clears
both PRESENT and PROT_NONE PTEs."
```

---

### Task 5: Fork/execve freelist handling

**Files:**
- Modify: `kernel/syscall/sys_process.c` — copy freelist on fork, reset on execve

**Context:** `sys_fork` copies PCB fields at lines 448-475. `sys_execve` resets `mmap_base` at line 843.

- [ ] **Step 1: Copy freelist on fork**

In `sys_fork`, after `child->mmap_base = parent->mmap_base;` (line 449), add:

```c
    __builtin_memcpy(child->mmap_free, parent->mmap_free,
                     parent->mmap_free_count * sizeof(mmap_free_t));
    child->mmap_free_count = parent->mmap_free_count;
```

- [ ] **Step 2: Copy freelist on clone (CLONE_VM thread path)**

In `sys_clone`, after `child->mmap_base = parent->mmap_base;` (line 235), add the same copy:

```c
    __builtin_memcpy(child->mmap_free, parent->mmap_free,
                     parent->mmap_free_count * sizeof(mmap_free_t));
    child->mmap_free_count = parent->mmap_free_count;
```

(Note: threads share the same address space via CLONE_VM, so they share the parent's PML4. The freelist copy here is for the child's own `aegis_process_t` struct. Since threads with CLONE_VM share `pml4_phys` but have separate PCBs, the freelist must be copied so both see the same state. On single-core, concurrent mutation is not possible.)

- [ ] **Step 3: Reset freelist on execve**

In `sys_execve`, after `proc->mmap_base = 0x0000700000000000ULL;` (line 843), add:

```c
    proc->mmap_free_count = 0;
```

- [ ] **Step 4: Build and test**

Run: `make test`
Expected: PASS.

Run: `python3 tests/test_threads.py`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add kernel/syscall/sys_process.c
git commit -m "feat: copy mmap freelist on fork/clone, reset on execve"
```

---

### Task 6: Test program and integration test

**Files:**
- Create: `user/mmap_test/main.c` — test binary
- Create: `user/mmap_test/Makefile` — build rules
- Create: `tests/test_mmap.py` — integration test
- Modify: `tests/run_tests.sh` — wire new test
- Modify: `Makefile` — add mmap_test to disk build

**Context:** The test binary must be statically linked with musl-gcc, placed on the ext2 disk at `/bin/mmap_test`. The integration test follows the same pattern as `test_threads.py`: boot vigil ISO with NVMe disk, log in, run the binary, check for success string.

- [ ] **Step 1: Create user/mmap_test/Makefile**

```makefile
CC      = musl-gcc
CFLAGS  = -static -O2 -fno-pie -no-pie -Wl,--build-id=none
TARGET  = mmap_test.elf

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
```

- [ ] **Step 2: Create user/mmap_test/main.c**

```c
#include <sys/mman.h>
#include <signal.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static volatile int got_signal = 0;
static sigjmp_buf jmpbuf;

static void segv_handler(int sig)
{
    (void)sig;
    got_signal = 1;
    siglongjmp(jmpbuf, 1);
}

int main(void)
{
    /* Test 1: VA reuse — munmap then mmap should recycle the address. */
    void *a1 = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (a1 == MAP_FAILED) {
        printf("MMAP FAIL: mmap1\n");
        return 1;
    }
    *(volatile int *)a1 = 99;
    munmap(a1, 4096);

    void *a2 = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (a2 == MAP_FAILED) {
        printf("MMAP FAIL: mmap2\n");
        return 1;
    }
    if (a2 != a1) {
        printf("MMAP FAIL: VA not reused a1=%p a2=%p\n", a1, a2);
        return 1;
    }

    /* Test 2: mprotect PROT_NONE — write should SIGSEGV. */
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        printf("MMAP FAIL: mmap3\n");
        return 1;
    }
    *(volatile int *)p = 42;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = segv_handler;
    sa.sa_flags   = 0;
    sigaction(SIGSEGV, &sa, NULL);

    mprotect(p, 4096, PROT_NONE);
    got_signal = 0;
    if (sigsetjmp(jmpbuf, 1) == 0) {
        *(volatile int *)p = 0;  /* should fault */
        printf("MMAP FAIL: PROT_NONE did not fault\n");
        return 1;
    }
    if (!got_signal) {
        printf("MMAP FAIL: no SIGSEGV for PROT_NONE\n");
        return 1;
    }

    /* Test 3: mprotect read-only — read should work, write should SIGSEGV. */
    void *r = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (r == MAP_FAILED) {
        printf("MMAP FAIL: mmap4\n");
        return 1;
    }
    *(volatile int *)r = 123;
    mprotect(r, 4096, PROT_READ);

    /* Read should succeed. */
    volatile int val = *(volatile int *)r;
    if (val != 123) {
        printf("MMAP FAIL: read-only read got %d\n", val);
        return 1;
    }

    /* Write should fault. */
    got_signal = 0;
    if (sigsetjmp(jmpbuf, 1) == 0) {
        *(volatile int *)r = 0;  /* should fault */
        printf("MMAP FAIL: PROT_READ did not fault on write\n");
        return 1;
    }
    if (!got_signal) {
        printf("MMAP FAIL: no SIGSEGV for PROT_READ write\n");
        return 1;
    }

    printf("MMAP OK\n");
    return 0;
}
```

- [ ] **Step 3: Add mmap_test to top-level Makefile**

In the `Makefile`, find the line that writes `thread_test` to the ext2 disk (near the `write user/thread_test/thread_test.elf /bin/thread_test` line) and add `mmap_test` on the same printf line, right after thread_test:

Add `write user/mmap_test/mmap_test.elf /bin/mmap_test\n` to the debugfs printf that populates `/bin/`.

Also add the build dependency: find the `user/thread_test/thread_test.elf` target and add a similar target for mmap_test:

```makefile
user/mmap_test/mmap_test.elf: user/mmap_test/main.c
	$(MAKE) -C user/mmap_test
```

And add `user/mmap_test/mmap_test.elf` to the `$(DISK)` target's prerequisite list (the same line that lists all user ELFs).

- [ ] **Step 4: Create tests/test_mmap.py**

```python
#!/usr/bin/env python3
"""test_mmap.py — Phase 30 mprotect + mmap freelist smoke test.

Boots Aegis with q35 + NVMe disk (ext2 with mmap_test binary),
logs in, runs /bin/mmap_test, checks for "MMAP OK" in output.

Skipped automatically if build/disk.img is not present.
"""
import subprocess, sys, os, select, fcntl, time, socket, tempfile

QEMU         = "qemu-system-x86_64"
ISO          = "build/aegis.iso"
DISK         = "build/disk.img"
BOOT_TIMEOUT = 120
CMD_TIMEOUT  = 30

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

_KEY_MAP = {
    ' ': 'spc', '\n': 'ret', '/': 'slash', '-': 'minus', '.': 'dot',
    ':': 'shift-semicolon', '|': 'shift-backslash', '_': 'shift-minus',
}
for c in 'abcdefghijklmnopqrstuvwxyz': _KEY_MAP[c] = c
for c in '0123456789': _KEY_MAP.setdefault(c, c)


def _type_string(mon_sock, s):
    for ch in s:
        key = _KEY_MAP.get(ch)
        if key is None:
            continue
        mon_sock.sendall(f'sendkey {key}\n'.encode())
        time.sleep(0.08)
        try:
            mon_sock.recv(4096)
        except OSError:
            pass


class SerialReader:
    def __init__(self, fd):
        self._fd = fd
        self._buf = b""

    def _drain(self, timeout=0.5):
        ready, _, _ = select.select([self._fd], [], [], timeout)
        if ready:
            try:
                chunk = os.read(self._fd, 65536)
                if chunk:
                    self._buf += chunk
            except (BlockingIOError, OSError):
                pass

    def wait_for(self, needle, deadline):
        enc = needle.encode()
        while time.time() < deadline:
            if enc in self._buf:
                return True
            self._drain()
        return enc in self._buf

    def full_output(self):
        return self._buf.decode("utf-8", errors="replace")


def build_iso():
    r = subprocess.run("make INIT=vigil iso", shell=True,
                       cwd=ROOT, capture_output=True)
    if r.returncode != 0:
        print("[FAIL] make INIT=vigil iso failed")
        print(r.stderr.decode())
        sys.exit(1)


def run_test():
    iso_path  = os.path.join(ROOT, ISO)
    disk_path = os.path.join(ROOT, DISK)

    if not os.path.exists(disk_path):
        print(f"SKIP: {DISK} not found — run 'make disk' first")
        sys.exit(0)

    build_iso()

    mon_path = tempfile.mktemp(suffix=".sock")
    proc = subprocess.Popen(
        [QEMU,
         "-machine", "q35", "-cpu", "Broadwell",
         "-cdrom", iso_path, "-boot", "order=d",
         "-display", "none", "-vga", "std", "-nodefaults",
         "-serial", "stdio", "-no-reboot", "-m", "256M",
         "-drive", f"file={disk_path},if=none,id=nvme0,format=raw",
         "-device", "nvme,drive=nvme0,serial=aegis0",
         "-device", "virtio-net-pci,netdev=n0,disable-legacy=on",
         "-netdev", "user,id=n0",
         "-monitor", f"unix:{mon_path},server,nowait"],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    flags = fcntl.fcntl(proc.stdout.fileno(), fcntl.F_GETFL)
    fcntl.fcntl(proc.stdout.fileno(), fcntl.F_SETFL, flags | os.O_NONBLOCK)
    serial = SerialReader(proc.stdout.fileno())

    deadline = time.time() + 10
    while not os.path.exists(mon_path) and time.time() < deadline:
        time.sleep(0.1)
    mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    mon.connect(mon_path)
    mon.setblocking(False)

    try:
        # Login
        if not serial.wait_for("login: ", time.time() + BOOT_TIMEOUT):
            print("FAIL: login prompt not found")
            sys.exit(1)
        _type_string(mon, "root\n")

        if not serial.wait_for("assword", time.time() + 10):
            print("FAIL: password prompt not found")
            sys.exit(1)
        _type_string(mon, "forevervigilant\n")

        if not serial.wait_for("# ", time.time() + 10):
            print("FAIL: shell prompt not found")
            sys.exit(1)

        # Run mmap test
        time.sleep(1)
        _type_string(mon, "/bin/mmap_test\n")

        if serial.wait_for("MMAP OK", time.time() + CMD_TIMEOUT):
            print("PASS: mmap_test reported MMAP OK")
        else:
            print("FAIL: 'MMAP OK' not found in output")
            print(f"  last 500 chars: {serial.full_output()[-500:]!r}")
            sys.exit(1)

    finally:
        try:
            mon.close()
        except OSError:
            pass
        proc.kill()
        proc.wait()
        try:
            os.unlink(mon_path)
        except OSError:
            pass


if __name__ == "__main__":
    run_test()
```

- [ ] **Step 5: Wire test_mmap.py into run_tests.sh**

In `tests/run_tests.sh`, after the `test_threads.py` line (line 131), add:

```bash

# Phase 30 mprotect + mmap freelist test — boots INIT=vigil with mmap_test
# on ext2 disk. Skipped automatically if build/disk.img is not present.
echo "--- test_mmap ---"
python3 tests/test_mmap.py
```

- [ ] **Step 6: Build disk and run test**

Run on remote:
```bash
make -C user/mmap_test
rm -f build/disk.img && make disk
make INIT=vigil iso
python3 tests/test_mmap.py
```
Expected: PASS: mmap_test reported MMAP OK

- [ ] **Step 7: Run full test suite**

Run: `make test` (with default INIT for boot oracle) then manually run all integration tests.
Expected: All tests PASS.

- [ ] **Step 8: Commit**

```bash
git add user/mmap_test/ tests/test_mmap.py tests/run_tests.sh Makefile
git commit -m "test: add mmap_test — VA reuse, PROT_NONE fault, read-only fault

Three tests: (1) munmap+mmap returns same VA (freelist), (2) mprotect
PROT_NONE causes SIGSEGV on write, (3) mprotect PROT_READ allows read
but SIGSEGV on write. Wired into run_tests.sh."
```

---

### Task 7: Update CLAUDE.md

**Files:**
- Modify: `.claude/CLAUDE.md` — build status table + roadmap + forward constraints

- [ ] **Step 1: Add Phase 30 to build status table**

Add after the Thread support entry:

```
| mprotect + mmap freelist (Phase 30) | ✅ | Real mprotect (W^X via NX); 64-slot VA freelist; test_mmap.py PASS |
```

- [ ] **Step 2: Update roadmap**

Change Phase 30 status from "Not started" to "✅ Done".

- [ ] **Step 3: Add forward constraints**

Add a Phase 30 forward constraints section:

1. **Freelist has no lock.** Safe on single-core. SMP requires spinlock.
2. **PROT_NONE pages still allocate physical frames.** Demand-paging deferred.
3. **No MAP_FIXED.** sys_mmap rejects addr!=0. Deferred.
4. **File-backed mmap and MAP_SHARED deferred.** No consumers until Phase 33/39.

- [ ] **Step 4: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs: update CLAUDE.md for Phase 30 mprotect + mmap freelist"
```
