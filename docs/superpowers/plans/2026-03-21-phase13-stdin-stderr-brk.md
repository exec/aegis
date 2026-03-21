# Phase 13: stdin + stderr + sys_brk Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add keyboard VFS driver (fd 0), stderr pre-open (fd 2), CAP_KIND_VFS_READ gate on sys_read, sys_brk syscall, and remove task_kbd — completing standard I/O and user-space heap before musl port.

**Architecture:** New `kbd_vfs.c` wraps `kbd_read()` in a stateless VFS singleton; `vmm_phys_of_user` + `vmm_unmap_user_page` extend vmm.c for per-PML4 walks; `elf_load` gains an `out_brk` out-param so `proc_spawn` initialises `proc->brk` at the top of loaded ELF segments.

**Tech Stack:** C (`-Wall -Wextra -Werror -ffreestanding`), `make test` oracle-diff harness, QEMU headless boot.

---

## File Structure

| File | Change | Why |
|------|--------|-----|
| `tests/expected/boot.txt` | Update oracle | RED first |
| `kernel/fs/kbd_vfs.h` | Create | kbd VFS interface |
| `kernel/fs/kbd_vfs.c` | Create | `kbd_vfs_read_fn` + stateless singleton |
| `kernel/cap/cap.h` | Add `CAP_KIND_VFS_READ 3u` | cap gate for sys_read |
| `kernel/mm/vmm.h` | Declare `vmm_phys_of_user` + `vmm_unmap_user_page` | sys_brk shrink path |
| `kernel/mm/vmm.c` | Implement those two helpers | same |
| `kernel/elf/elf.h` | Add `uint64_t *out_brk` to `elf_load` | heap start tracking |
| `kernel/elf/elf.c` | Track highest PT_LOAD VA → `*out_brk` | same |
| `kernel/proc/proc.h` | Add `brk` field to `aegis_process_t` | per-process heap break |
| `kernel/proc/proc.c` | VFS_READ grant; fd 0/2 pre-open; set `proc->brk` | spawn init |
| `kernel/syscall/syscall.c` | Cap gate on `sys_read`; add `sys_brk` (case 12) | capability model |
| `kernel/core/main.c` | Remove `task_kbd` + its `sched_spawn` call | task_kbd retired |
| `Makefile` | Add `kbd_vfs.c` to `FS_SRCS` | new source file |
| `user/init/main.c` | Add `sys_brk` helper + heap test | oracle `[HEAP]` line |
| `.claude/CLAUDE.md` | Update build status table | Phase 13 done |

---

## Task 1: Update Oracle (RED)

**Files:**
- Modify: `tests/expected/boot.txt`

The test diff will fail until implementation is complete. Updating it first establishes the RED state per project methodology.

Three changes from Phase 12 oracle:
1. `2 capabilities` → `3 capabilities`
2. `3 tasks` → `2 tasks`
3. Add `[HEAP] OK: brk works` after `[MOTD]`

- [ ] **Step 1: Update boot.txt**

Replace `tests/expected/boot.txt` with:

```
[SERIAL] OK: COM1 initialized at 115200 baud
[VGA] OK: text mode 80x25
[PMM] OK: 127MB usable across 2 regions
[VMM] OK: kernel mapped to 0xFFFFFFFF80000000
[VMM] OK: mapped-window allocator active
[KVA] OK: kernel virtual allocator active
[CAP] OK: capability subsystem initialized
[IDT] OK: 48 vectors installed
[PIC] OK: IRQ0-15 remapped to vectors 0x20-0x2F
[PIT] OK: timer at 100 Hz
[KBD] OK: PS/2 keyboard ready
[GDT] OK: ring 3 descriptors installed
[TSS] OK: RSP0 initialized
[SYSCALL] OK: SYSCALL/SYSRET enabled
[SMAP] OK: supervisor access prevention active
[VFS] OK: initialized
[INITRD] OK: 1 file registered
[CAP] OK: 3 capabilities granted to init
[VMM] OK: identity map removed
[SCHED] OK: scheduler started, 2 tasks
[MOTD] Hello from initrd!
[HEAP] OK: brk works
[AEGIS] System halted.
```

- [ ] **Step 2: Verify test is now RED**

```bash
make test
```

Expected: FAIL (oracle mismatch — that's correct, implementation not done yet).

- [ ] **Step 3: Commit**

```bash
git add tests/expected/boot.txt
git commit -m "test: update oracle for Phase 13 (RED)"
```

---

## Task 2: Keyboard VFS Driver

**Files:**
- Create: `kernel/fs/kbd_vfs.h`
- Create: `kernel/fs/kbd_vfs.c`

`kbd_vfs_read_fn` receives a **kernel** buffer — `sys_read` calls the driver with its own stack buffer and copies to user space via `copy_to_user` after return. The driver never touches user memory directly (same pattern as `initrd_read_fn`). The `write` stub returns `ENOSYS` (not NULL) so `sys_write(0,...)` gets a proper error instead of `EBADF`.

- [ ] **Step 1: Create `kernel/fs/kbd_vfs.h`**

```c
#ifndef KBD_VFS_H
#define KBD_VFS_H

#include "vfs.h"

/* kbd_vfs_open — return a pointer to the static keyboard vfs_file_t.
 * Used by proc_spawn to pre-populate fd 0 (stdin).
 * The keyboard hardware is already initialised by kbd_init() in kernel_main;
 * no separate kbd_vfs_init() step is needed. */
vfs_file_t *kbd_vfs_open(void);

#endif /* KBD_VFS_H */
```

- [ ] **Step 2: Create `kernel/fs/kbd_vfs.c`**

```c
#include "kbd_vfs.h"
#include "vfs.h"
#include "kbd.h"       /* kbd_read() */
#include <stdint.h>

static int
kbd_vfs_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
    (void)priv; (void)off;  /* stream device — offset ignored */
    uint64_t i;
    char *kbuf = (char *)buf;
    /* kbd_read() blocks until a key is available. buf is a kernel buffer
     * (sys_read's stack); sys_read copies to user space via copy_to_user. */
    for (i = 0; i < len; i++)
        kbuf[i] = kbd_read();
    return (int)len;
}

static int
kbd_vfs_write_fn(void *priv, const void *buf, uint64_t len)
{
    (void)priv; (void)buf; (void)len;
    return -38; /* ENOSYS — stdin is not writable */
}

static void
kbd_vfs_close_fn(void *priv)
{
    (void)priv; /* stateless singleton — nothing to release */
}

static const vfs_ops_t s_kbd_ops = {
    .read  = kbd_vfs_read_fn,
    .write = kbd_vfs_write_fn,
    .close = kbd_vfs_close_fn,
};

static vfs_file_t s_kbd_file = {
    .ops    = &s_kbd_ops,
    .priv   = (void *)0,
    .offset = 0,
};

vfs_file_t *
kbd_vfs_open(void)
{
    return &s_kbd_file;
}
```

- [ ] **Step 3: Verify it compiles in isolation**

```bash
x86_64-elf-gcc -ffreestanding -nostdlib -nostdinc \
    -isystem $(x86_64-elf-gcc -print-file-name=include) \
    -mcmodel=kernel -fno-pie -fno-pic \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -fno-stack-protector -Wall -Wextra -Werror \
    -Ikernel/arch/x86_64 -Ikernel/core -Ikernel/fs \
    -c kernel/fs/kbd_vfs.c -o /dev/null
```

Expected: no output (clean compile).

- [ ] **Step 4: Commit**

```bash
git add kernel/fs/kbd_vfs.h kernel/fs/kbd_vfs.c
git commit -m "feat: add keyboard VFS driver (kbd_vfs)"
```

---

## Task 3: New Cap Constant

**Files:**
- Modify: `kernel/cap/cap.h`

- [ ] **Step 1: Add `CAP_KIND_VFS_READ` to cap.h**

Read `kernel/cap/cap.h` first to find the exact line after `CAP_KIND_VFS_WRITE`. Add immediately after it:

```c
#define CAP_KIND_VFS_READ  3u   /* permission to call sys_read */
```

- [ ] **Step 2: Verify cap.h builds**

```bash
make 2>&1 | head -5
```

Expected: build proceeds (cap.h is a header-only change, compilation of consumers validates it).

- [ ] **Step 3: Commit**

```bash
git add kernel/cap/cap.h
git commit -m "feat: add CAP_KIND_VFS_READ constant"
```

---

## Task 4: VMM Helpers for sys_brk Shrink Path

**Files:**
- Modify: `kernel/mm/vmm.h`
- Modify: `kernel/mm/vmm.c`

`sys_brk`'s shrink path must walk a *specific process's* PML4 (not the active master PML4). `vmm_phys_of` uses `s_pml4_phys` (the active kernel PML4); these two new helpers take an explicit `pml4_phys`. Both follow the **walk-overwrite** pattern from `vmm_phys_of` and `vmm_unmap_page` (overwrite window PTE at each level, single `vmm_window_unmap` at end).

`vmm_phys_of_user` returns 0 if not mapped (graceful; the grow path may not have covered the exact page). `vmm_unmap_user_page` skips silently if the PTE is absent.

- [ ] **Step 1: Add declarations to `kernel/mm/vmm.h`**

Add after the `vmm_free_user_pml4` declaration:

```c
/* vmm_phys_of_user — walk pml4_phys to find the physical address mapped at virt.
 * Returns physical address, or 0 if not mapped.
 * Uses the window allocator. Safe to call with any PML4 (not just active CR3). */
uint64_t vmm_phys_of_user(uint64_t pml4_phys, uint64_t virt);

/* vmm_unmap_user_page — clear the PTE for virt in pml4_phys and invlpg.
 * Does not free the physical page. Caller frees via pmm_free_page.
 * Silent no-op if the page is not mapped. */
void vmm_unmap_user_page(uint64_t pml4_phys, uint64_t virt);
```

- [ ] **Step 2: Implement both in `kernel/mm/vmm.c`**

Add after `vmm_free_user_pml4` (at end of file):

```c
uint64_t
vmm_phys_of_user(uint64_t pml4_phys, uint64_t virt)
{
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    /* Walk-overwrite pattern: overwrite window PTE at each level, single unmap. */
    uint64_t *pml4  = vmm_window_map(pml4_phys);
    uint64_t  pml4e = pml4[pml4_idx];
    if (!(pml4e & VMM_FLAG_PRESENT)) { vmm_window_unmap(); return 0; }

    uint64_t *pdpt  = vmm_window_map(PTE_ADDR(pml4e));
    uint64_t  pdpte = pdpt[pdpt_idx];
    if (!(pdpte & VMM_FLAG_PRESENT)) { vmm_window_unmap(); return 0; }

    uint64_t *pd  = vmm_window_map(PTE_ADDR(pdpte));
    uint64_t  pde = pd[pd_idx];
    if (!(pde & VMM_FLAG_PRESENT) || (pde & PTE_PS)) {
        vmm_window_unmap(); return 0;
    }

    uint64_t *pt  = vmm_window_map(PTE_ADDR(pde));
    uint64_t  pte = pt[pt_idx];
    vmm_window_unmap();

    return (pte & VMM_FLAG_PRESENT) ? PTE_ADDR(pte) : 0;
}

void
vmm_unmap_user_page(uint64_t pml4_phys, uint64_t virt)
{
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    /* Walk-overwrite pattern. Silent no-op if any level is absent. */
    uint64_t *pml4  = vmm_window_map(pml4_phys);
    uint64_t  pml4e = pml4[pml4_idx];
    if (!(pml4e & VMM_FLAG_PRESENT)) { vmm_window_unmap(); return; }

    uint64_t *pdpt  = vmm_window_map(PTE_ADDR(pml4e));
    uint64_t  pdpte = pdpt[pdpt_idx];
    if (!(pdpte & VMM_FLAG_PRESENT)) { vmm_window_unmap(); return; }

    uint64_t *pd  = vmm_window_map(PTE_ADDR(pdpte));
    uint64_t  pde = pd[pd_idx];
    if (!(pde & VMM_FLAG_PRESENT) || (pde & PTE_PS)) {
        vmm_window_unmap(); return;
    }

    uint64_t *pt  = vmm_window_map(PTE_ADDR(pde));
    if (pt[pt_idx] & VMM_FLAG_PRESENT)
        pt[pt_idx] = 0;
    vmm_window_unmap();
    arch_vmm_invlpg(virt);
}
```

- [ ] **Step 3: Build to verify no compile errors**

```bash
make 2>&1 | grep -E "error:|warning:" | head -10
```

Expected: no output.

- [ ] **Step 4: Commit**

```bash
git add kernel/mm/vmm.h kernel/mm/vmm.c
git commit -m "feat: add vmm_phys_of_user and vmm_unmap_user_page"
```

---

## Task 5: elf_load Out-Param

**Files:**
- Modify: `kernel/elf/elf.h`
- Modify: `kernel/elf/elf.c`

`proc_spawn` needs the exact top-of-ELF-segments VA (page-rounded) to initialise `proc->brk`. The ELF loader already iterates all PT_LOAD segments — adding `out_brk` tracking costs one comparison per segment.

- [ ] **Step 1: Update `kernel/elf/elf.h`**

Replace:
```c
uint64_t elf_load(uint64_t pml4_phys, const uint8_t *data, size_t len);
```

With:
```c
/* elf_load — parse ELF64, map all PT_LOAD segments into pml4_phys.
 * Returns entry RIP on success, 0 on parse error.
 * *out_brk is set to the first page-aligned VA above all loaded segments.
 * Used by proc_spawn to initialise proc->brk. */
uint64_t elf_load(uint64_t pml4_phys, const uint8_t *data,
                  size_t len, uint64_t *out_brk);
```

- [ ] **Step 2: Update `kernel/elf/elf.c`**

In `elf_load`, add `uint64_t seg_end = 0;` before the loop. Inside the loop, after `if (ph->p_type != PT_LOAD) continue;`, add:

```c
        uint64_t this_end = ph->p_vaddr + ph->p_memsz;
        if (this_end > seg_end)
            seg_end = this_end;
```

After the loop, before `return eh->e_entry;`, add:

```c
    *out_brk = (seg_end + 4095UL) & ~4095UL;
```

Full updated function body for reference:

```c
uint64_t
elf_load(uint64_t pml4_phys, const uint8_t *data, size_t len, uint64_t *out_brk)
{
    (void)len;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;

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
    uint64_t seg_end = 0;
    for (i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD)
            continue;

        uint64_t this_end = ph->p_vaddr + ph->p_memsz;
        if (this_end > seg_end)
            seg_end = this_end;

        uint64_t page_count = (ph->p_memsz + 4095UL) / 4096UL;
        uint64_t j;

        uint8_t *dst = kva_alloc_pages(page_count);

        const uint8_t *src = data + ph->p_offset;
        uint64_t k;
        for (k = 0; k < ph->p_filesz; k++)
            dst[k] = src[k];
        for (k = ph->p_filesz; k < ph->p_memsz; k++)
            dst[k] = 0;

        uint64_t map_flags = VMM_FLAG_USER;
        if (ph->p_flags & PF_W)
            map_flags |= VMM_FLAG_WRITABLE;

        for (j = 0; j < page_count; j++) {
            vmm_map_user_page(pml4_phys,
                              ph->p_vaddr + j * 4096UL,
                              kva_page_phys(dst + j * 4096UL),
                              map_flags);
        }
    }

    *out_brk = (seg_end + 4095UL) & ~4095UL;
    return eh->e_entry;
}
```

- [ ] **Step 3: Build — expect one error about call-site in proc.c**

```bash
make 2>&1 | grep "error:" | head -5
```

Expected: one error like `too few arguments to function 'elf_load'` in `proc.c`. That's correct — fix it in Task 6.

- [ ] **Step 4: Commit (even with the expected error — it's a deliberate in-progress state)**

Actually, commit only after fixing the call site in Task 6. Skip this step; combine with Task 6 commit.

---

## Task 6: proc.h + proc.c Changes

**Files:**
- Modify: `kernel/proc/proc.h`
- Modify: `kernel/proc/proc.c`

Three changes to `proc.h`: add `brk` field. Keep `#include "kbd_vfs.h"` in `proc.c` only (not `proc.h`) — `proc.h` already gets `vfs_file_t` from `vfs.h`.

Four changes to `proc.c`: update `elf_load` call site; grant `CAP_KIND_VFS_READ`; pre-open fd 0 (stdin) and fd 2 (stderr); set `proc->brk`.

- [ ] **Step 1: Add `brk` field to `kernel/proc/proc.h`**

In `aegis_process_t`, add `brk` after `caps`:

```c
typedef struct {
    aegis_task_t  task;                    /* MUST be first */
    uint64_t      pml4_phys;
    vfs_file_t    fds[PROC_MAX_FDS];
    cap_slot_t    caps[CAP_TABLE_SIZE];
    uint64_t      brk;    /* current heap limit (user VA); grows up */
} aegis_process_t;
```

Do NOT add `#include "kbd_vfs.h"` to proc.h — that include goes in proc.c only.

- [ ] **Step 2: Update `kernel/proc/proc.c`**

Add `#include "kbd_vfs.h"` to the includes at top of `proc.c`.

Update the `elf_load` call site to pass `&brk_start`:

```c
    uint64_t brk_start;
    uint64_t entry_rip = elf_load(proc->pml4_phys, elf_data, elf_len, &brk_start);
    if (!entry_rip) {
        printk("[PROC] FAIL: ELF parse error\n");
        for (;;) {}
    }
```

After the existing two `cap_grant` calls (VFS_OPEN and VFS_WRITE), add a third:

```c
    /* Grant read capability. */
    if (cap_grant(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_READ, CAP_RIGHTS_READ) < 0) {
        printk("[CAP] FAIL: cap_grant VFS_READ returned -ENOCAP\n");
        for (;;) {}
    }
```

Update the printk line from `"2 capabilities"` to `"3 capabilities"`:

```c
    printk("[CAP] OK: 3 capabilities granted to init\n");
```

After the existing `proc->fds[1] = *console_open();` line, add fd 0 and fd 2:

```c
    /* Pre-open fd 0 (stdin) to keyboard device. */
    proc->fds[0] = *kbd_vfs_open();

    /* Pre-open fd 2 (stderr) to console device. */
    proc->fds[2] = *console_open();
```

After the pre-opens, set brk:

```c
    /* Initialise heap break to top of ELF segments. */
    proc->brk = brk_start;
```

- [ ] **Step 3: Build**

```bash
make 2>&1 | grep "error:" | head -10
```

Expected: no errors.

- [ ] **Step 4: Commit**

```bash
git add kernel/proc/proc.h kernel/proc/proc.c kernel/elf/elf.h kernel/elf/elf.c
git commit -m "feat: elf_load out_brk param; proc brk field; fd 0/2 pre-open; 3 caps"
```

---

## Task 7: syscall.c — sys_read Cap Gate + sys_brk

**Files:**
- Modify: `kernel/syscall/syscall.c`

Two changes:
1. `sys_read` gets a `CAP_KIND_VFS_READ` gate at the top (Phase 11 oversight fix).
2. New `sys_brk` function + `case 12` in dispatch.

`sys_brk` always page-aligns `arg1` upward before the grow/shrink loop so `proc->brk` stays page-aligned. This is musl-compatible: musl expects the kernel to return the actual (possibly rounded-up) break.

- [ ] **Step 1: Add missing includes to syscall.c**

Read the current includes in `syscall.c`. Add any missing ones:
- `"vmm.h"` (for `vmm_map_user_page`, `vmm_phys_of_user`, `vmm_unmap_user_page`)
- `"pmm.h"` (for `pmm_alloc_page`, `pmm_free_page`)

- [ ] **Step 2: Add cap gate to `sys_read`**

At the **top** of `sys_read`, before the existing `if (arg1 >= PROC_MAX_FDS)` check, add:

```c
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_READ, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)ENOCAP;
```

Then update the existing `aegis_process_t *proc` declaration that follows — remove the duplicate (the existing `sys_read` doesn't currently declare `proc`, so just add the block above the `PROC_MAX_FDS` check, and change the `vfs_file_t *f` line to use the already-declared `proc`).

Full updated `sys_read`:

```c
static uint64_t
sys_read(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_VFS_READ, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    if (arg1 >= PROC_MAX_FDS)
        return (uint64_t)-9;   /* EBADF */
    vfs_file_t *f = &proc->fds[arg1];
    if (!f->ops)
        return (uint64_t)-9;   /* EBADF */
    if (!user_ptr_valid(arg2, arg3))
        return (uint64_t)-14;  /* EFAULT */

    char kbuf[256];
    uint64_t total = 0;
    while (total < arg3) {
        uint64_t n = arg3 - total;
        if (n > sizeof(kbuf)) n = sizeof(kbuf);
        int got = f->ops->read(f->priv, kbuf, f->offset, n);
        if (got <= 0) break;
        copy_to_user((void *)(uintptr_t)(arg2 + total), kbuf, (uint64_t)got);
        f->offset += (uint64_t)got;
        total     += (uint64_t)got;
    }
    return total;
}
```

- [ ] **Step 3: Add `sys_brk` function before `syscall_dispatch`**

```c
/*
 * sys_brk — syscall 12
 *
 * arg1 = requested new break address (0 = query current brk)
 *
 * Returns the new (or current) break address.
 * On OOM, returns the current break unchanged (Linux-compatible).
 * No capability gate — process expands its own address space only.
 *
 * arg1 is page-aligned upward before processing. proc->brk is always
 * page-aligned. musl's malloc passes exact byte offsets and expects the
 * kernel to return the actual (rounded-up) new break — this is correct.
 */
static uint64_t
sys_brk(uint64_t arg1)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();

    if (arg1 == 0)
        return proc->brk;  /* query */

    /* Clamp to user address space */
    if (arg1 >= 0x00007FFFFFFFFFFFULL)
        return proc->brk;

    /* Page-align upward so proc->brk is always page-aligned */
    arg1 = (arg1 + 4095UL) & ~4095UL;

    if (arg1 > proc->brk) {
        /* Grow: map pages [proc->brk, arg1) into this process's PML4 */
        uint64_t va;
        for (va = proc->brk; va < arg1; va += 4096UL) {
            uint64_t phys = pmm_alloc_page();
            if (!phys)
                return proc->brk;  /* OOM — return current brk unchanged */
            vmm_map_user_page(proc->pml4_phys, va, phys,
                              VMM_FLAG_PRESENT | VMM_FLAG_USER |
                              VMM_FLAG_WRITABLE);
        }
        proc->brk = arg1;
    } else if (arg1 < proc->brk) {
        /* Shrink: unmap and free pages [arg1, proc->brk) */
        uint64_t va;
        for (va = arg1; va < proc->brk; va += 4096UL) {
            uint64_t phys = vmm_phys_of_user(proc->pml4_phys, va);
            if (phys) {
                vmm_unmap_user_page(proc->pml4_phys, va);
                pmm_free_page(phys);
            }
        }
        proc->brk = arg1;
    }

    return proc->brk;
}
```

- [ ] **Step 4: Add `case 12` to `syscall_dispatch`**

```c
    case 12: return sys_brk(arg1);
```

- [ ] **Step 5: Build**

```bash
make 2>&1 | grep "error:" | head -10
```

Expected: no errors.

- [ ] **Step 6: Commit**

```bash
git add kernel/syscall/syscall.c
git commit -m "feat: cap gate on sys_read; add sys_brk (syscall 12)"
```

---

## Task 8: Remove task_kbd from main.c

**Files:**
- Modify: `kernel/core/main.c`

`task_kbd` competed with user-space stdin for the same `kbd_read()` ring buffer. Now that fd 0 is owned by the user process, `task_kbd` is retired. Task count: 3 → 2.

- [ ] **Step 1: Remove `task_kbd` from main.c**

Delete the entire `task_kbd` function (the static function `task_kbd(void)` and its body).

Delete the `sched_spawn(task_kbd);` call.

Keep `sched_spawn(task_idle);` — `task_idle` is the permanent kernel idle task.

Also remove the `#include` for `kbd.h` from main.c if it was only there for `task_kbd`. Read main.c first to check — `kbd_init()` is still called so `kbd.h` is still needed.

- [ ] **Step 2: Verify task_kbd is gone**

```bash
grep -n 'task_kbd' kernel/core/main.c
```

Expected: no output.

- [ ] **Step 3: Build**

```bash
make 2>&1 | grep "error:" | head -10
```

Expected: no errors.

- [ ] **Step 4: Commit**

```bash
git add kernel/core/main.c
git commit -m "feat: retire task_kbd; user process owns stdin"
```

---

## Task 9: Makefile — Add kbd_vfs.c

**Files:**
- Modify: `Makefile`

- [ ] **Step 1: Add `kbd_vfs.c` to `FS_SRCS`**

Change:
```makefile
FS_SRCS = \
    kernel/fs/vfs.c \
    kernel/fs/initrd.c \
    kernel/fs/console.c
```

To:
```makefile
FS_SRCS = \
    kernel/fs/vfs.c \
    kernel/fs/initrd.c \
    kernel/fs/console.c \
    kernel/fs/kbd_vfs.c
```

- [ ] **Step 2: Build**

```bash
make 2>&1 | grep "error:" | head -10
```

Expected: no errors. Full kernel builds including kbd_vfs.c.

- [ ] **Step 3: Commit**

```bash
git add Makefile
git commit -m "build: add kbd_vfs.c to FS_SRCS"
```

---

## Task 10: user/init/main.c — Heap Test

**Files:**
- Modify: `user/init/main.c`

Add a `sys_brk` inline syscall helper and a heap test that:
1. Calls `sys_brk(0)` to get current break.
2. Calls `sys_brk(heap + 4096)` to allocate one page.
3. Writes `"[HEAP] OK: brk works\n"` into heap memory manually (no libc).
4. Calls `sys_write(1, p, i)` to print it.

The `[HEAP]` prefix ensures the line passes the `grep ^[` filter in the test harness.

- [ ] **Step 1: Update `user/init/main.c`**

```c
static inline long
sys_open(const char *path, long flags, long mode)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(2L), "D"(path), "S"(flags), "d"(mode)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long
sys_read(long fd, void *buf, long count)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(0L), "D"(fd), "S"(buf), "d"(count)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long
sys_write(long fd, const void *buf, long count)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(1L), "D"(fd), "S"(buf), "d"(count)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long
sys_close(long fd)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(3L), "D"(fd)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long
sys_brk(long addr)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(12L), "D"(addr)
        : "rcx", "r11", "memory");
    return ret;
}

static inline void
sys_exit(long code)
{
    __asm__ volatile ("syscall"
        : : "a"(60L), "D"(code) : "rcx", "r11", "memory");
}

void
_start(void)
{
    /* Read and print /etc/motd (tests VFS + sys_read + sys_write). */
    char buf[64];
    long fd = sys_open("/etc/motd", 0, 0);
    long n  = sys_read(fd, buf, (long)sizeof(buf));
    sys_write(1, buf, n);
    sys_close(fd);

    /* Test sys_brk: allocate one heap page, write a string, print it.
     * Uses byte-by-byte copy — no memcpy/strcpy (freestanding, no libc). */
    long heap = sys_brk(0);        /* query current brk */
    sys_brk(heap + 4096);          /* allocate one page */
    char *p = (char *)heap;
    const char msg[] = "[HEAP] OK: brk works\n";
    int i;
    for (i = 0; msg[i]; i++)
        p[i] = msg[i];
    sys_write(1, p, i);

    sys_exit(0);
}
```

Read the current `user/init/main.c` first to see if syscall wrappers already exist. If they do, only add `sys_brk` and update `_start`. Avoid duplicating existing wrappers.

- [ ] **Step 2: Build the full kernel (rebuilds user binary + init_bin.c)**

```bash
make 2>&1 | grep "error:" | head -10
```

Expected: no errors.

- [ ] **Step 3: Commit**

```bash
git add user/init/main.c
git commit -m "feat: add sys_brk test to user init; writes [HEAP] line"
```

---

## Task 11: GREEN — make test

**Files:** None (verification only)

- [ ] **Step 1: Run make test**

```bash
make test
```

Expected: exit 0 with output matching the new oracle exactly.

- [ ] **Step 2: If FAIL — diagnose**

```bash
make test 2>&1
```

Common failure modes:
- `[CAP] OK: 2 capabilities` — proc.c printk not updated (Task 6)
- `[SCHED] OK: scheduler started, 3 tasks` — task_kbd not removed (Task 8) or sched_spawn count wrong
- `[HEAP] OK: brk works` missing — sys_brk not dispatched or user/init not calling it
- Build error — check Makefile FS_SRCS (Task 9)
- Extra output lines — some new printk was added accidentally

If the output is close but wrong, compare:
```bash
make iso && qemu-system-x86_64 -machine pc -cdrom build/aegis.iso -boot order=d \
    -display none -vga std -nodefaults -serial stdio -no-reboot -m 128M \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 2>/dev/null \
    | sed 's/\x1b\[[0-9;]*[mGKH]//g' | grep '^\['
```

Compare that output against `tests/expected/boot.txt` to find the exact mismatch.

- [ ] **Step 3: Commit (if any fixes were needed)**

```bash
git add -p   # stage only the fix
git commit -m "fix: <describe what was wrong>"
```

---

## Task 12: Success Verification Audits

- [ ] **Step 1: Run all oracle checks from spec**

```bash
# kbd VFS driver exists
ls kernel/fs/kbd_vfs.c kernel/fs/kbd_vfs.h

# CAP_KIND_VFS_READ defined
grep -n 'CAP_KIND_VFS_READ' kernel/cap/cap.h

# sys_read has cap_check gate
grep -n 'cap_check' kernel/syscall/syscall.c

# sys_brk registered in dispatch
grep -n 'sys_brk' kernel/syscall/syscall.c

# task_kbd removed
grep -n 'task_kbd' kernel/core/main.c  # must produce NO output

# brk field in proc.h
grep -n 'brk' kernel/proc/proc.h
```

Expected output:
```
kernel/fs/kbd_vfs.c  kernel/fs/kbd_vfs.h
kernel/cap/cap.h:N:#define CAP_KIND_VFS_READ  3u
kernel/syscall/syscall.c:N:    if (cap_check(proc->caps, ...  [at least 3 lines]
kernel/syscall/syscall.c:N:sys_brk  [at least 2 lines]
(no output for task_kbd)
kernel/proc/proc.h:N:    uint64_t      brk;
```

- [ ] **Step 2: Run make test one final time**

```bash
make test
```

Expected: exit 0.

---

## Task 13: Update CLAUDE.md

**Files:**
- Modify: `.claude/CLAUDE.md`

- [ ] **Step 1: Update Build Status table**

Mark Phase 13 as done. Add row:

```
| stdin/stderr/sys_brk (Phase 13) | ✅ Done | kbd VFS (fd 0); stderr (fd 2); sys_brk; CAP_KIND_VFS_READ; task_kbd removed |
```

Update the "Last updated" line at the bottom:

```
*Last updated: 2026-03-21 — Phase 13 complete, make test GREEN. kbd VFS driver; fd 0/2 pre-open; CAP_KIND_VFS_READ gate on sys_read; sys_brk (syscall 12); task_kbd retired; task count 2.*
```

Add Phase 14 forward-looking constraints section with content from spec:

```markdown
### Phase 13 forward-looking constraints

**`sys_brk` page-aligns the break.** `proc->brk` is always page-aligned after grow or shrink. User-space allocators (musl's `malloc`) pass exact byte offsets and expect the kernel to return the actual rounded-up break — Phase 13 rounds up, which musl handles correctly.

**fd 0 blocks on `kbd_read()`.** A user process calling `sys_read(0, ...)` will block until a key is pressed. In headless `make test` there is no keyboard input — `init` must not call `sys_read(0, ...)`. Phase 14 or later should provide a `kbd_poll`-based non-blocking path or `sys_poll`.

**No `sys_mmap`.** musl's allocator falls back to `mmap(MAP_ANONYMOUS)` if `brk` fails. Phase 13 provides no `mmap`. A musl port requires either a brk-only allocator config or a minimal `sys_mmap` in Phase 14.

**Capability delegation deferred.** A second user process receives capabilities via `proc_spawn` grants only. `sys_cap_grant` for parent→child delegation remains future work.
```

- [ ] **Step 2: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs: update CLAUDE.md for Phase 13 completion"
```
