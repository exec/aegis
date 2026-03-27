# Phase 31: /proc Filesystem + VMA Tracking — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add per-process VMA tracking and a capability-gated `/proc` virtual filesystem providing process introspection and system info.

**Architecture:** VMA table is a dynamically-allocated sorted array (kva page, 170 slots) tracked per-process with refcounting for CLONE_VM threads. Procfs is a VFS backend registered by path prefix in `vfs_open()`/`vfs_stat_path()`. Content is generated on open into a kva buffer, read from buffer, freed on close.

**Tech Stack:** C kernel code; Rust capability constant; musl-gcc user test binary; Python integration test.

**Critical build note:** The Makefile lacks `-MMD` header dependency tracking. After modifying any `.h` file, you MUST use `rm -rf build && make` (or `make clean && make`) to ensure all dependent `.c` files are recompiled. Incremental builds after header changes will use stale `.o` files and produce broken binaries.

**Remote build:** This project builds and tests on `dylan@10.0.0.19` via SSH key `/Users/dylan/.ssh/aegis/id_ed25519`. Local macOS cannot build. Always `rm -rf build` before `make` when headers change.

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `kernel/mm/vma.h` | Create | VMA type definitions, function declarations |
| `kernel/mm/vma.c` | Create | VMA operations: init, insert, remove, update_prot, clear, clone, free |
| `kernel/fs/procfs.h` | Create | Procfs declarations: procfs_init, procfs_open, procfs_stat |
| `kernel/fs/procfs.c` | Create | Procfs VFS backend: path routing, content generators, dir listings |
| `kernel/proc/proc.h` | Modify | Add vma fields + exe_path to aegis_process_t; declare proc_find_by_pid |
| `kernel/proc/proc.c` | Modify | Initialize vma fields in proc_spawn; implement proc_find_by_pid |
| `kernel/mm/pmm.h` | Modify | Declare pmm_total_pages, pmm_free_pages |
| `kernel/mm/pmm.c` | Modify | Implement pmm_total_pages, pmm_free_pages |
| `kernel/cap/cap.h` | Modify | Add CAP_KIND_PROC_READ = 10 |
| `kernel/cap/src/lib.rs` | Modify | Add CAP_KIND_PROC_READ constant (for documentation; Rust doesn't use it directly) |
| `kernel/elf/elf.c` | Modify | Call vma_insert for each PT_LOAD segment |
| `kernel/syscall/sys_memory.c` | Modify | Call vma_insert/remove/update in mmap/munmap/mprotect/brk |
| `kernel/syscall/sys_process.c` | Modify | vma_clone in fork, vma share in clone, vma_clear+exe_path in execve, grant PROC_READ cap |
| `kernel/fs/vfs.c` | Modify | Add /proc/ prefix dispatch in vfs_open and vfs_stat_path |
| `Makefile` | Modify | Add vma.c to MM_SRCS, procfs.c to FS_SRCS, proc_test to DISK_USER_BINS |
| `user/proc_test/main.c` | Create | Test binary exercising /proc/self/maps, status, meminfo, stat, fd |
| `user/proc_test/Makefile` | Create | musl-gcc static build |
| `tests/test_proc.py` | Create | Integration test: boot, login, run proc_test, check PROC OK |
| `tests/run_tests.sh` | Modify | Add test_proc.py |

---

### Task 1: VMA Infrastructure (vma.h + vma.c)

**Files:**
- Create: `kernel/mm/vma.h`
- Create: `kernel/mm/vma.c`
- Modify: `Makefile` (add vma.c to MM_SRCS)

- [ ] **Step 1: Create vma.h with type definitions and function declarations**

```c
/* kernel/mm/vma.h — per-process Virtual Memory Area tracking */
#ifndef AEGIS_VMA_H
#define AEGIS_VMA_H

#include <stdint.h>

/* VMA type constants */
#define VMA_NONE         0
#define VMA_ELF_TEXT     1   /* PT_LOAD with PROT_EXEC */
#define VMA_ELF_DATA     2   /* PT_LOAD without PROT_EXEC */
#define VMA_HEAP         3   /* [brk_base..brk] */
#define VMA_STACK        4   /* user stack */
#define VMA_MMAP         5   /* anonymous mmap */
#define VMA_THREAD_STACK 6   /* thread stack via pthread_create */
#define VMA_GUARD        7   /* guard page (PROT_NONE) */

typedef struct {
    uint64_t base;
    uint64_t len;
    uint32_t prot;    /* PROT_READ | PROT_WRITE | PROT_EXEC */
    uint8_t  type;    /* VMA_* constant */
    uint8_t  _pad[3];
} vma_entry_t;  /* 24 bytes */

/* Forward-declare to avoid circular include with proc.h */
struct aegis_process;

/* vma_init — allocate a kva page for the VMA table.
 * Sets proc->vma_table, vma_count=0, vma_capacity=170, vma_refcount=1. */
void vma_init(struct aegis_process *proc);

/* vma_insert — add a VMA entry sorted by base address.
 * Merges with adjacent entries if same prot+type. */
void vma_insert(struct aegis_process *proc,
                uint64_t base, uint64_t len, uint32_t prot, uint8_t type);

/* vma_remove — remove [base, base+len) from VMA table.
 * Splits entries at boundaries if partial overlap. */
void vma_remove(struct aegis_process *proc, uint64_t base, uint64_t len);

/* vma_update_prot — change permissions for [base, base+len).
 * Splits entries at boundaries if needed. */
void vma_update_prot(struct aegis_process *proc,
                     uint64_t base, uint64_t len, uint32_t new_prot);

/* vma_clear — set count to 0 (called by execve). */
void vma_clear(struct aegis_process *proc);

/* vma_clone — deep copy VMA table from src to dst (for fork).
 * Allocates a new kva page for dst, copies entries. */
void vma_clone(struct aegis_process *dst, struct aegis_process *src);

/* vma_share — share VMA table from parent to child (for CLONE_VM threads).
 * Increments refcount, copies pointer. */
void vma_share(struct aegis_process *child, struct aegis_process *parent);

/* vma_free — decrement refcount; free kva page if refcount reaches 0. */
void vma_free(struct aegis_process *proc);

#endif /* AEGIS_VMA_H */
```

- [ ] **Step 2: Create vma.c with all VMA operations**

```c
/* kernel/mm/vma.c — per-process VMA tracking */
#include "vma.h"
#include "../proc/proc.h"
#include "kva.h"
#include <stdint.h>
#include <stddef.h>

#define VMA_PAGE_CAPACITY (4096 / sizeof(vma_entry_t))  /* 170 */

void
vma_init(struct aegis_process *proc)
{
    proc->vma_table = kva_alloc_pages(1);
    proc->vma_count = 0;
    proc->vma_capacity = (uint32_t)VMA_PAGE_CAPACITY;
    proc->vma_refcount = 1;
}

/* Insert at position idx, shifting entries right. */
static void
vma_shift_right(vma_entry_t *table, uint32_t count, uint32_t idx)
{
    uint32_t i;
    for (i = count; i > idx; i--)
        table[i] = table[i - 1];
}

/* Remove entry at idx, shifting entries left. */
static void
vma_shift_left(vma_entry_t *table, uint32_t *count, uint32_t idx)
{
    uint32_t i;
    for (i = idx; i < *count - 1; i++)
        table[i] = table[i + 1];
    (*count)--;
}

void
vma_insert(struct aegis_process *proc,
           uint64_t base, uint64_t len, uint32_t prot, uint8_t type)
{
    if (!proc->vma_table || len == 0) return;

    vma_entry_t *t = proc->vma_table;
    uint32_t n = proc->vma_count;

    /* Find insertion point (sorted by base) */
    uint32_t pos = 0;
    while (pos < n && t[pos].base < base)
        pos++;

    /* Try merge with previous entry */
    if (pos > 0 && t[pos - 1].prot == prot && t[pos - 1].type == type &&
        t[pos - 1].base + t[pos - 1].len == base) {
        t[pos - 1].len += len;
        /* Check if we can also merge with next */
        if (pos < n && t[pos].prot == prot && t[pos].type == type &&
            t[pos - 1].base + t[pos - 1].len == t[pos].base) {
            t[pos - 1].len += t[pos].len;
            vma_shift_left(t, &proc->vma_count, pos);
        }
        return;
    }

    /* Try merge with next entry */
    if (pos < n && t[pos].prot == prot && t[pos].type == type &&
        base + len == t[pos].base) {
        t[pos].base = base;
        t[pos].len += len;
        return;
    }

    /* No merge — insert new entry */
    if (n >= proc->vma_capacity) return;  /* table full */

    vma_shift_right(t, n, pos);
    t[pos].base = base;
    t[pos].len  = len;
    t[pos].prot = prot;
    t[pos].type = type;
    t[pos]._pad[0] = t[pos]._pad[1] = t[pos]._pad[2] = 0;
    proc->vma_count++;
}

void
vma_remove(struct aegis_process *proc, uint64_t base, uint64_t len)
{
    if (!proc->vma_table || len == 0) return;

    uint64_t end = base + len;
    vma_entry_t *t = proc->vma_table;
    uint32_t i = 0;

    while (i < proc->vma_count) {
        vma_entry_t *e = &t[i];
        uint64_t e_end = e->base + e->len;

        /* No overlap */
        if (e_end <= base || e->base >= end) {
            i++;
            continue;
        }

        /* Fully contained — remove entire entry */
        if (e->base >= base && e_end <= end) {
            vma_shift_left(t, &proc->vma_count, i);
            continue;  /* don't increment i — next entry shifted into this slot */
        }

        /* Remove from start of entry */
        if (base <= e->base && end < e_end) {
            e->len = e_end - end;
            e->base = end;
            i++;
            continue;
        }

        /* Remove from end of entry */
        if (base > e->base && end >= e_end) {
            e->len = base - e->base;
            i++;
            continue;
        }

        /* Remove from middle — split into two */
        if (base > e->base && end < e_end) {
            if (proc->vma_count >= proc->vma_capacity) {
                i++;
                continue;  /* can't split, table full */
            }
            /* Create right half after the split */
            uint64_t right_base = end;
            uint64_t right_len  = e_end - end;
            uint32_t right_prot = e->prot;
            uint8_t  right_type = e->type;

            /* Shrink left half */
            e->len = base - e->base;

            /* Insert right half */
            vma_shift_right(t, proc->vma_count, i + 1);
            t[i + 1].base = right_base;
            t[i + 1].len  = right_len;
            t[i + 1].prot = right_prot;
            t[i + 1].type = right_type;
            t[i + 1]._pad[0] = t[i + 1]._pad[1] = t[i + 1]._pad[2] = 0;
            proc->vma_count++;
            i += 2;
            continue;
        }

        i++;
    }
}

void
vma_update_prot(struct aegis_process *proc,
                uint64_t base, uint64_t len, uint32_t new_prot)
{
    if (!proc->vma_table || len == 0) return;

    uint64_t end = base + len;
    vma_entry_t *t = proc->vma_table;
    uint32_t i = 0;

    while (i < proc->vma_count) {
        vma_entry_t *e = &t[i];
        uint64_t e_end = e->base + e->len;

        /* No overlap */
        if (e_end <= base || e->base >= end) {
            i++;
            continue;
        }

        /* Already same prot */
        if (e->prot == new_prot) {
            i++;
            continue;
        }

        /* Fully contained — just change prot */
        if (e->base >= base && e_end <= end) {
            e->prot = new_prot;
            i++;
            continue;
        }

        /* Partial overlap at start of entry (base <= e->base, end < e_end) */
        if (base <= e->base && end < e_end) {
            if (proc->vma_count >= proc->vma_capacity) { i++; continue; }
            /* Split: [e->base..end) gets new_prot, [end..e_end) keeps old */
            uint64_t old_len = e_end - end;
            uint32_t old_prot = e->prot;
            uint8_t  old_type = e->type;

            e->len = end - e->base;
            e->prot = new_prot;

            vma_shift_right(t, proc->vma_count, i + 1);
            t[i + 1].base = end;
            t[i + 1].len  = old_len;
            t[i + 1].prot = old_prot;
            t[i + 1].type = old_type;
            t[i + 1]._pad[0] = t[i + 1]._pad[1] = t[i + 1]._pad[2] = 0;
            proc->vma_count++;
            i += 2;
            continue;
        }

        /* Partial overlap at end of entry (base > e->base, end >= e_end) */
        if (base > e->base && end >= e_end) {
            if (proc->vma_count >= proc->vma_capacity) { i++; continue; }
            uint64_t new_len = e_end - base;
            uint8_t  e_type = e->type;

            e->len = base - e->base;  /* shrink left part */

            vma_shift_right(t, proc->vma_count, i + 1);
            t[i + 1].base = base;
            t[i + 1].len  = new_len;
            t[i + 1].prot = new_prot;
            t[i + 1].type = e_type;
            t[i + 1]._pad[0] = t[i + 1]._pad[1] = t[i + 1]._pad[2] = 0;
            proc->vma_count++;
            i += 2;
            continue;
        }

        /* Middle split: base > e->base && end < e_end */
        if (base > e->base && end < e_end) {
            if (proc->vma_count + 2 > proc->vma_capacity) { i++; continue; }
            uint64_t right_base = end;
            uint64_t right_len  = e_end - end;
            uint32_t old_prot   = e->prot;
            uint8_t  e_type     = e->type;

            e->len = base - e->base;  /* left part keeps old prot */

            /* Insert middle (new_prot) and right (old_prot) */
            vma_shift_right(t, proc->vma_count, i + 1);
            vma_shift_right(t, proc->vma_count + 1, i + 2);
            t[i + 1].base = base;
            t[i + 1].len  = len;
            t[i + 1].prot = new_prot;
            t[i + 1].type = e_type;
            t[i + 1]._pad[0] = t[i + 1]._pad[1] = t[i + 1]._pad[2] = 0;
            t[i + 2].base = right_base;
            t[i + 2].len  = right_len;
            t[i + 2].prot = old_prot;
            t[i + 2].type = e_type;
            t[i + 2]._pad[0] = t[i + 2]._pad[1] = t[i + 2]._pad[2] = 0;
            proc->vma_count += 2;
            i += 3;
            continue;
        }

        i++;
    }
}

void
vma_clear(struct aegis_process *proc)
{
    if (proc->vma_table)
        proc->vma_count = 0;
}

void
vma_clone(struct aegis_process *dst, struct aegis_process *src)
{
    if (!src->vma_table) {
        dst->vma_table    = (vma_entry_t *)0;
        dst->vma_count    = 0;
        dst->vma_capacity = 0;
        dst->vma_refcount = 0;
        return;
    }
    dst->vma_table = kva_alloc_pages(1);
    dst->vma_count    = src->vma_count;
    dst->vma_capacity = (uint32_t)VMA_PAGE_CAPACITY;
    dst->vma_refcount = 1;
    uint32_t i;
    for (i = 0; i < src->vma_count; i++)
        dst->vma_table[i] = src->vma_table[i];
}

void
vma_share(struct aegis_process *child, struct aegis_process *parent)
{
    child->vma_table    = parent->vma_table;
    child->vma_count    = parent->vma_count;
    child->vma_capacity = parent->vma_capacity;
    parent->vma_refcount++;
    child->vma_refcount = parent->vma_refcount;
}

void
vma_free(struct aegis_process *proc)
{
    if (!proc->vma_table) return;

    if (proc->vma_refcount <= 1) {
        kva_free_pages(proc->vma_table, 1);
    } else {
        proc->vma_refcount--;
    }
    proc->vma_table = (vma_entry_t *)0;
    proc->vma_count = 0;
}
```

- [ ] **Step 3: Add vma.c to Makefile MM_SRCS**

In `Makefile`, change:
```
MM_SRCS = \
    kernel/mm/pmm.c \
    kernel/mm/vmm.c \
    kernel/mm/kva.c
```
to:
```
MM_SRCS = \
    kernel/mm/pmm.c \
    kernel/mm/vmm.c \
    kernel/mm/kva.c \
    kernel/mm/vma.c
```

- [ ] **Step 4: Build to verify no compilation errors**

Run: `rm -rf build && make`
Expected: Clean build (no errors, no warnings).

- [ ] **Step 5: Commit**

```bash
git add kernel/mm/vma.h kernel/mm/vma.c Makefile
git commit -m "feat: add VMA tracking infrastructure (vma.h + vma.c)"
```

---

### Task 2: Process Struct Changes + Initialization

**Files:**
- Modify: `kernel/proc/proc.h` (add VMA fields, exe_path, proc_find_by_pid)
- Modify: `kernel/proc/proc.c` (initialize VMA fields in proc_spawn, implement proc_find_by_pid)

- [ ] **Step 1: Add VMA fields and exe_path to aegis_process_t in proc.h**

In `kernel/proc/proc.h`, add the include at the top (after existing includes):
```c
#include "vma.h"
```

Note: `vma.h` forward-declares `struct aegis_process` to avoid circular includes. The `aegis_process_t` typedef is defined below in proc.h. The functions in `vma.h` take `struct aegis_process *` which resolves after the typedef since `vma.c` includes `proc.h`.

In the `aegis_process_t` struct, after the `mmap_free_count` field and before `pid`, add:

```c
    vma_entry_t  *vma_table;              /* kva-allocated VMA table; NULL until vma_init */
    uint32_t      vma_count;              /* number of valid VMA entries */
    uint32_t      vma_capacity;           /* max entries (170 per kva page) */
    uint32_t      vma_refcount;           /* 1 = sole owner; >1 = shared (CLONE_VM) */
    char          exe_path[256];          /* binary path, set at execve */
```

Add declaration before `#endif`:
```c
/* Find a user process by PID. Walks the scheduler circular task list.
 * Returns NULL if no matching process found. */
aegis_process_t *proc_find_by_pid(uint32_t pid);
```

**Important:** The `struct aegis_process` forward declaration in `vma.h` must match the typedef name. Since `aegis_process_t` is defined as `typedef struct { ... } aegis_process_t;` (anonymous struct), the forward declaration won't match. Fix: change the typedef in proc.h from anonymous to named:

```c
typedef struct aegis_process {
    /* ... all existing fields ... */
} aegis_process_t;
```

This is a one-word change (add `aegis_process` after `struct`) that makes the forward declaration in `vma.h` work.

- [ ] **Step 2: Initialize VMA fields in proc_spawn in proc.c**

In `kernel/proc/proc.c`, add include at the top:
```c
#include "vma.h"
```

In `proc_spawn()`, after `proc->mmap_free_count = 0;` (line ~239), add:
```c
    /* Initialize VMA tracking */
    vma_init(proc);
    /* Record user stack VMA */
    vma_insert(proc, USER_STACK_BASE, USER_STACK_NPAGES * 4096ULL,
               0x01 | 0x02,  /* PROT_READ | PROT_WRITE */
               VMA_STACK);
    /* exe_path starts empty; set by execve */
    proc->exe_path[0] = '\0';
```

Also add ELF segment VMAs after `elf_load` succeeds (after line ~79 `uint64_t brk_start = er.brk;`). Note: `elf_load` does not return per-segment info, only the aggregate `brk`. For init, we know the ELF is loaded starting at some VA. The simplest approach: we don't record ELF VMAs in proc_spawn since init is loaded before we have a way to enumerate segments. Instead, record ELF VMAs from inside `elf_load` itself (Task 3). For proc_spawn, just record the stack.

Set exe_path for init:
```c
    __builtin_memcpy(proc->exe_path, "/bin/init", 10);
```
(Place this after `proc->exe_path[0] = '\0';`)

- [ ] **Step 3: Implement proc_find_by_pid in proc.c**

At the bottom of `kernel/proc/proc.c`, add:

```c
aegis_process_t *
proc_find_by_pid(uint32_t pid)
{
    aegis_task_t *cur = sched_current();
    if (!cur) return (aegis_process_t *)0;

    /* Check current task first */
    if (cur->is_user) {
        aegis_process_t *p = (aegis_process_t *)cur;
        if (p->pid == pid)
            return p;
    }

    /* Walk the circular list */
    aegis_task_t *t = cur->next;
    while (t != cur) {
        if (t->is_user) {
            aegis_process_t *p = (aegis_process_t *)t;
            if (p->pid == pid)
                return p;
        }
        t = t->next;
    }
    return (aegis_process_t *)0;
}
```

- [ ] **Step 4: Build to verify compilation**

Run: `rm -rf build && make`
Expected: Clean build.

- [ ] **Step 5: Commit**

```bash
git add kernel/proc/proc.h kernel/proc/proc.c
git commit -m "feat: add VMA fields and exe_path to aegis_process_t"
```

---

### Task 3: Wire VMA Tracking Into ELF Loader, mmap, munmap, mprotect, brk

**Files:**
- Modify: `kernel/elf/elf.c` (add vma_insert for each PT_LOAD)
- Modify: `kernel/syscall/sys_memory.c` (add vma calls in mmap, munmap, mprotect, brk)

- [ ] **Step 1: Add VMA recording to elf_load**

In `kernel/elf/elf.c`, add includes at the top:
```c
#include "../mm/vma.h"
#include "../proc/proc.h"
#include "../sched/sched.h"
```

Inside the PT_LOAD loop (after the `vmm_map_user_page` loop, before the closing `}` of the `if (ph->p_type == PT_LOAD)` block — around line 152), add:

```c
        /* Record VMA for this segment */
        {
            aegis_task_t *cur = sched_current();
            if (cur && cur->is_user) {
                aegis_process_t *p = (aegis_process_t *)cur;
                uint32_t seg_prot = 0x01;  /* PROT_READ always */
                if (ph->p_flags & PF_W)
                    seg_prot |= 0x02;  /* PROT_WRITE */
                if (ph->p_flags & 1)  /* PF_X */
                    seg_prot |= 0x04;  /* PROT_EXEC */
                uint8_t seg_type = (ph->p_flags & 1) ? VMA_ELF_TEXT : VMA_ELF_DATA;
                vma_insert(p, va_base, page_count * 4096UL, seg_prot, seg_type);
            }
        }
```

Note: During `proc_spawn`, `sched_current()` is not the new process (it hasn't been added yet). So `elf_load` during `proc_spawn` won't record VMAs — they're recorded later when `sys_execve` calls `elf_load` (where `sched_current()` IS the calling process). For `proc_spawn`, we accept that init's ELF VMAs are not tracked (init is a special case). Alternatively, pass the process pointer to elf_load — but that changes the API. Simpler: skip the VMA insert if `sched_current()` is not the target. Since proc_spawn is only called once at boot, this is acceptable.

Actually, a cleaner approach: **pass a `struct aegis_process *` to elf_load** so it always knows which process to record VMAs for. But that changes the elf.h API which other code depends on. The pragmatic choice: only record VMAs when called from sys_execve (sched_current is valid). proc_spawn's init process will have stack VMA but not ELF VMAs — acceptable, since /proc is not yet mounted at that point.

- [ ] **Step 2: Add VMA calls to sys_mmap**

In `kernel/syscall/sys_memory.c`, add include at the top:
```c
#include "../mm/vma.h"
```

In `sys_mmap`, after the successful return at the bottom (after `proc->mmap_base = base + len;` / before `return base;`), add:
```c
    vma_insert(proc, base, len,
               (uint32_t)(arg3 & 0x07),  /* prot bits: PROT_READ|WRITE|EXEC */
               VMA_MMAP);
```

The `(aegis_process_t *)` is already available as `proc`.

- [ ] **Step 3: Add VMA calls to sys_munmap**

In `sys_munmap`, after `mmap_free_insert(proc, arg1, len);` and before `return 0;`, add:
```c
    vma_remove(proc, arg1, len);
```

- [ ] **Step 4: Add VMA calls to sys_mprotect**

In `sys_mprotect`, after the `vmm_set_user_prot` loop (after the `for` loop ends, before `return 0;`), add:
```c
    vma_update_prot(proc, addr, rlen, (uint32_t)(prot & 0x07));
```

- [ ] **Step 5: Add VMA calls to sys_brk**

In `sys_brk`, in the grow path (inside `if (arg1 > proc->brk)`), after `proc->brk = arg1;`, add:
```c
        /* Update VMA: find existing heap entry or create one */
        {
            uint32_t i;
            int found = 0;
            for (i = 0; i < proc->vma_count; i++) {
                if (proc->vma_table && proc->vma_table[i].type == VMA_HEAP) {
                    proc->vma_table[i].len = arg1 - proc->vma_table[i].base;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                /* First brk grow: record the heap VMA starting at the old brk */
                vma_insert(proc, proc->brk - (arg1 - proc->brk),
                           arg1 - (proc->brk - (arg1 - proc->brk)),
                           0x01 | 0x02, VMA_HEAP);
            }
        }
```

Wait — that's getting complicated. The old brk was already updated by `proc->brk = arg1` by this point. Let's restructure: capture old_brk before the update.

Before the `if (arg1 > proc->brk)` block (around line 32), add:
```c
    uint64_t old_brk = proc->brk;
```

Then in the grow path, after `proc->brk = arg1;`, add:
```c
        /* Update heap VMA */
        if (proc->vma_table) {
            uint32_t vi;
            int found = 0;
            for (vi = 0; vi < proc->vma_count; vi++) {
                if (proc->vma_table[vi].type == VMA_HEAP) {
                    proc->vma_table[vi].len = proc->brk - proc->vma_table[vi].base;
                    found = 1;
                    break;
                }
            }
            if (!found)
                vma_insert(proc, old_brk, proc->brk - old_brk, 0x01 | 0x02, VMA_HEAP);
        }
```

In the shrink path, after `proc->brk = arg1;`, add:
```c
        /* Update heap VMA */
        if (proc->vma_table) {
            uint32_t vi;
            for (vi = 0; vi < proc->vma_count; vi++) {
                if (proc->vma_table[vi].type == VMA_HEAP) {
                    if (proc->brk <= proc->vma_table[vi].base) {
                        /* Heap fully shrunk — remove entry */
                        vma_remove(proc, proc->vma_table[vi].base,
                                   proc->vma_table[vi].len);
                    } else {
                        proc->vma_table[vi].len = proc->brk - proc->vma_table[vi].base;
                    }
                    break;
                }
            }
        }
```

- [ ] **Step 6: Build to verify**

Run: `rm -rf build && make`
Expected: Clean build.

- [ ] **Step 7: Commit**

```bash
git add kernel/elf/elf.c kernel/syscall/sys_memory.c
git commit -m "feat: wire VMA tracking into ELF loader, mmap, munmap, mprotect, brk"
```

---

### Task 4: Wire VMA Into fork, clone, execve + Add CAP_KIND_PROC_READ

**Files:**
- Modify: `kernel/syscall/sys_process.c` (vma_clone in fork, vma_share in clone, vma_clear in execve, exe_path, PROC_READ cap)
- Modify: `kernel/cap/cap.h` (add CAP_KIND_PROC_READ)
- Modify: `kernel/cap/src/lib.rs` (add comment for PROC_READ)
- Modify: `kernel/proc/proc.c` (grant PROC_READ cap to init)

- [ ] **Step 1: Add CAP_KIND_PROC_READ to cap.h**

In `kernel/cap/cap.h`, after `CAP_KIND_THREAD_CREATE`:
```c
#define CAP_KIND_PROC_READ  10u  /* may read /proc/[other-pid]/* */
```

- [ ] **Step 2: Add comment to lib.rs**

In `kernel/cap/src/lib.rs`, add a comment near the top (after the `ENOCAP` constant):
```rust
// CAP_KIND_PROC_READ = 10 — defined in cap.h; Rust side does not
// reference it directly. cap_check validates it generically.
```

- [ ] **Step 3: Grant PROC_READ to init in proc.c**

In `kernel/proc/proc.c`, in `proc_spawn()`, after the NET_ADMIN cap_grant (around line 303), add:
```c
    /* Grant proc_read capability — required for reading other processes' /proc. */
    if (cap_grant(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_PROC_READ, CAP_RIGHTS_READ) < 0) {
        printk("[CAP] FAIL: cap_grant PROC_READ returned -ENOCAP\n");
        for (;;) {}
    }
```

Update the printk count message from `"8 capabilities"` to `"9 capabilities"`:
```c
    printk("[CAP] OK: 9 capabilities granted to init\n");
```

- [ ] **Step 4: Grant PROC_READ in execve baseline**

In `kernel/syscall/sys_process.c`, in `sys_execve`, in the capability reset block (around line 864), after the `CAP_KIND_THREAD_CREATE` grant, add:
```c
        cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_PROC_READ, CAP_RIGHTS_READ);
```

- [ ] **Step 5: Add VMA include to sys_process.c**

At the top of `kernel/syscall/sys_process.c`, add:
```c
#include "../mm/vma.h"
```

- [ ] **Step 6: Add vma_clone to sys_fork**

In `sys_fork`, after the `mmap_free_count` copy (around line 455), add:
```c
    vma_clone(child, parent);
    __builtin_memcpy(child->exe_path, parent->exe_path, sizeof(parent->exe_path));
```

- [ ] **Step 7: Add vma_share to sys_clone**

In `sys_clone` (thread creation path), after the `mmap_free_count` copy (around line 238), add:
```c
    vma_share(child, parent);
    __builtin_memcpy(child->exe_path, parent->exe_path, sizeof(parent->exe_path));
```

- [ ] **Step 8: Add vma_clear and exe_path to sys_execve**

In `sys_execve`, in the "Reset heap/mmap/TLS state" section (around line 848-851), after `proc->mmap_free_count = 0;`, add:
```c
    vma_clear(proc);
```

Also set exe_path from the resolved path. After `vma_clear(proc);`:
```c
    {
        uint64_t pi;
        for (pi = 0; pi < sizeof(proc->exe_path) - 1 && path[pi]; pi++)
            proc->exe_path[pi] = path[pi];
        proc->exe_path[pi] = '\0';
    }
```

After the user stack mapping in execve (after the `vmm_map_user_page` loop for stack pages, around line 911), add:
```c
    /* Record user stack VMA */
    vma_insert(proc, USER_STACK_BASE_EXEC, USER_STACK_NPAGES * 4096ULL,
               0x01 | 0x02, VMA_STACK);
```

Note: `USER_STACK_BASE_EXEC` and `USER_STACK_TOP_EXEC` are already defined in sys_process.c. Find their values:

```c
#define USER_STACK_TOP_EXEC   0x7FFFFFFF000ULL
#define USER_STACK_NPAGES     4ULL
#define USER_STACK_BASE_EXEC  (USER_STACK_TOP_EXEC - USER_STACK_NPAGES * 4096ULL)
```

If `USER_STACK_BASE_EXEC` is not defined, use `(USER_STACK_TOP_EXEC - USER_STACK_NPAGES * 4096ULL)`.

- [ ] **Step 9: Add vma_free to process exit**

Search for where processes are freed (in `sched_exit` or the waitpid zombie reap path). In `sys_process.c`, in the `sys_waitpid` function where the zombie child is reaped (where `vmm_free_user_pml4` is called), add before the `kva_free_pages(child, 1)` call:
```c
            vma_free(child);
```

Also in `sched.c` or wherever `sched_exit` frees resources, check if vma_free is needed. Since sched_exit makes the task a zombie (doesn't free it), and the actual freeing is in waitpid, the vma_free in waitpid is sufficient.

For the `exit_group` path in sys_clone (thread exit), add `vma_free()` where thread PCBs are freed.

- [ ] **Step 10: Update tests/expected/boot.txt**

Change the init capability count from `8` to `9`:
```
[CAP] OK: 9 capabilities granted to init
```

- [ ] **Step 11: Build and run make test**

Run: `rm -rf build && make test`
Expected: PASS (boot.txt matches with updated cap count).

- [ ] **Step 12: Commit**

```bash
git add kernel/cap/cap.h kernel/cap/src/lib.rs kernel/proc/proc.c \
        kernel/syscall/sys_process.c tests/expected/boot.txt
git commit -m "feat: wire VMA into fork/clone/execve + add CAP_KIND_PROC_READ"
```

---

### Task 5: PMM Stats (pmm_total_pages, pmm_free_pages)

**Files:**
- Modify: `kernel/mm/pmm.h` (declare functions)
- Modify: `kernel/mm/pmm.c` (implement functions)

- [ ] **Step 1: Add declarations to pmm.h**

In `kernel/mm/pmm.h`, before `#endif`:
```c
/* pmm_total_pages — return total managed physical pages. */
uint64_t pmm_total_pages(void);

/* pmm_free_pages — return count of currently free physical pages.
 * Scans the bitmap; O(n) where n = PMM_MAX_PAGES/8. */
uint64_t pmm_free_pages(void);
```

- [ ] **Step 2: Implement in pmm.c**

PMM uses a static global `total_bytes` calculated in `pmm_init()`. We need to save this. In `pmm.c`, add a file-scoped variable after `pmm_bitmap`:
```c
static uint64_t s_total_managed_pages;
```

At the end of `pmm_init()`, after the printk (around line 132), add:
```c
    /* Save total managed page count for pmm_total_pages(). */
    {
        uint64_t count = 0;
        for (uint64_t i = 0; i < PMM_MAX_PAGES / 8; i++) {
            uint8_t byte = pmm_bitmap[i];
            /* Count free pages in initial state is hard — count total instead. */
        }
    }
```

Actually, simpler: `total_bytes` is already computed in `pmm_init` as the sum of all usable regions. Save it:

```c
static uint64_t s_total_usable_bytes;
```

At the end of `pmm_init()`, after the loop that calculates `total_bytes` but before the printk:
```c
    s_total_usable_bytes = total_bytes;
```

Then implement the functions at the bottom of pmm.c:
```c
uint64_t
pmm_total_pages(void)
{
    return s_total_usable_bytes / PAGE_SIZE;
}

uint64_t
pmm_free_pages(void)
{
    uint64_t free_count = 0;
    for (uint64_t i = 0; i < PMM_MAX_PAGES / 8; i++) {
        uint8_t byte = pmm_bitmap[i];
        /* Count zero bits (free pages) */
        uint8_t alloc = byte;
        while (alloc) {
            free_count--;  /* cancel out the unconditional add below */
            alloc &= (alloc - 1);  /* clear lowest set bit */
        }
        free_count += 8;  /* assume all 8 free, subtract allocated above */
    }
    /* Cap to total managed pages (bitmap covers 4GB but not all is usable) */
    if (free_count > pmm_total_pages())
        free_count = pmm_total_pages();
    return free_count;
}
```

Wait, that double-counts pages outside managed regions. Better approach — just count set bits (allocated) and subtract from total:

```c
uint64_t
pmm_free_pages(void)
{
    uint64_t allocated = 0;
    for (uint64_t i = 0; i < PMM_MAX_PAGES / 8; i++) {
        uint8_t byte = pmm_bitmap[i];
        while (byte) {
            allocated++;
            byte &= (byte - 1);
        }
    }
    uint64_t total = PMM_MAX_PAGES;
    if (allocated > total)
        return 0;
    /* The bitmap covers 4GB (PMM_MAX_PAGES). Managed pages is a subset.
     * Reserved pages (outside usable regions) are set in the bitmap.
     * Free pages = total usable - (allocated usable).
     * Since reserved pages are always set and usable pages start cleared,
     * allocated_usable = allocated - reserved = allocated - (PMM_MAX_PAGES - s_total_usable_bytes/PAGE_SIZE).
     * free = total_usable - allocated_usable = s_total_usable_bytes/PAGE_SIZE - allocated + (PMM_MAX_PAGES - s_total_usable_bytes/PAGE_SIZE)
     *       = PMM_MAX_PAGES - allocated
     * This works because bitmap covers exactly PMM_MAX_PAGES. */
    return total - allocated;
}
```

Hmm, that's also wrong because the bitmap initially marks everything as reserved (0xFF), then frees usable regions. So free = unset bits in the bitmap. Simply count unset bits:

```c
uint64_t
pmm_free_pages(void)
{
    uint64_t free_count = 0;
    for (uint64_t i = 0; i < PMM_MAX_PAGES / 8; i++) {
        uint8_t byte = pmm_bitmap[i];
        /* Count zero bits (free pages) using popcount of inverted byte */
        uint8_t inv = (uint8_t)~byte;
        while (inv) {
            free_count++;
            inv &= (inv - 1);
        }
    }
    return free_count;
}
```

This is simple and correct.

- [ ] **Step 3: Build to verify**

Run: `rm -rf build && make`
Expected: Clean build.

- [ ] **Step 4: Commit**

```bash
git add kernel/mm/pmm.h kernel/mm/pmm.c
git commit -m "feat: add pmm_total_pages and pmm_free_pages for /proc/meminfo"
```

---

### Task 6: Procfs VFS Backend

**Files:**
- Create: `kernel/fs/procfs.h`
- Create: `kernel/fs/procfs.c`
- Modify: `kernel/fs/vfs.c` (add /proc/ dispatch)
- Modify: `Makefile` (add procfs.c to FS_SRCS)

- [ ] **Step 1: Create procfs.h**

```c
/* kernel/fs/procfs.h — /proc virtual filesystem */
#ifndef AEGIS_PROCFS_H
#define AEGIS_PROCFS_H

#include "vfs.h"

/* procfs_init — called from vfs_init(). No-op for now (no global state). */
void procfs_init(void);

/* procfs_open — open a /proc file. path is relative (after "/proc/").
 * Returns 0 on success, -2 (ENOENT) if not found. */
int procfs_open(const char *path, int flags, vfs_file_t *out);

/* procfs_stat — stat a /proc file. path is the full path starting with "/proc".
 * Returns 0 on success, -2 (ENOENT) if not found. */
int procfs_stat(const char *path, k_stat_t *out);

#endif /* AEGIS_PROCFS_H */
```

- [ ] **Step 2: Create procfs.c with content generators and VFS ops**

This is the largest file. Key sections:
- Helper: `procfs_parse_pid` — parse "self" or numeric pid from path
- Helper: `procfs_lookup_proc` — resolve pid to aegis_process_t*
- Content generators: `gen_maps`, `gen_status`, `gen_stat`, `gen_exe`, `gen_cmdline`, `gen_meminfo`, `gen_version`
- VFS ops: `procfs_read_fn`, `procfs_close_fn`, `procfs_readdir_fn`, `procfs_stat_fn`
- Path dispatch: `procfs_open`

```c
/* kernel/fs/procfs.c — /proc virtual filesystem */
#include "procfs.h"
#include "vfs.h"
#include "../proc/proc.h"
#include "../sched/sched.h"
#include "../mm/pmm.h"
#include "../mm/vma.h"
#include "../mm/kva.h"
#include "../cap/cap.h"
#include "../core/printk.h"
#include <stdint.h>
#include <stddef.h>

/* ── string helpers ──────────────────────────────────────────────────── */

static int
pfs_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static char *
pfs_u64_hex(char *p, uint64_t v, int min_digits)
{
    char tmp[16];
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else { while (v) { tmp[n++] = "0123456789abcdef"[v & 0xF]; v >>= 4; } }
    while (n < min_digits) tmp[n++] = '0';
    for (int i = n - 1; i >= 0; i--) *p++ = tmp[i];
    return p;
}

static char *
pfs_u64_dec(char *p, uint64_t v)
{
    char tmp[20];
    int n = 0;
    if (v == 0) { *p++ = '0'; return p; }
    while (v) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    for (int i = n - 1; i >= 0; i--) *p++ = tmp[i];
    return p;
}

static char *
pfs_strcpy(char *dst, const char *src)
{
    while (*src) *dst++ = *src++;
    return dst;
}

static uint64_t
pfs_strlen(const char *s)
{
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

/* ── pid parsing ─────────────────────────────────────────────────────── */

/* Parse numeric PID from string. Returns 0 on failure. */
static uint32_t
parse_pid_str(const char *s, const char **end_out)
{
    uint32_t pid = 0;
    while (*s >= '0' && *s <= '9') {
        pid = pid * 10 + (uint32_t)(*s - '0');
        s++;
    }
    if (end_out) *end_out = s;
    return pid;
}

/* ── content generators ──────────────────────────────────────────────── */

/* All generators write into a 4096-byte buffer and return bytes written. */

static uint32_t
gen_maps(char *buf, uint32_t bufsz, aegis_process_t *proc)
{
    char *p = buf;
    char *end = buf + bufsz - 1;
    uint32_t i;

    if (!proc->vma_table) {
        *p = '\0';
        return 0;
    }

    for (i = 0; i < proc->vma_count && p < end - 80; i++) {
        vma_entry_t *v = &proc->vma_table[i];
        uint64_t vend = v->base + v->len;

        /* address range */
        p = pfs_u64_hex(p, v->base, 12);
        *p++ = '-';
        p = pfs_u64_hex(p, vend, 12);
        *p++ = ' ';

        /* permissions */
        *p++ = (v->prot & 0x01) ? 'r' : '-';
        *p++ = (v->prot & 0x02) ? 'w' : '-';
        *p++ = (v->prot & 0x04) ? 'x' : '-';
        *p++ = 'p';  /* always private */
        *p++ = ' ';

        /* offset dev inode */
        p = pfs_strcpy(p, "00000000 00:00 0");

        /* name */
        switch (v->type) {
        case VMA_HEAP:         p = pfs_strcpy(p, "          [heap]"); break;
        case VMA_STACK:        p = pfs_strcpy(p, "          [stack]"); break;
        case VMA_GUARD:        p = pfs_strcpy(p, "          [guard]"); break;
        case VMA_THREAD_STACK: p = pfs_strcpy(p, "          [thread_stack]"); break;
        case VMA_ELF_TEXT:
        case VMA_ELF_DATA:
            if (proc->exe_path[0]) {
                *p++ = ' ';
                p = pfs_strcpy(p, proc->exe_path);
            }
            break;
        default: break;
        }
        *p++ = '\n';
    }
    *p = '\0';
    return (uint32_t)(p - buf);
}

static const char *
task_state_str(uint32_t state)
{
    switch (state) {
    case 0: return "R (running)";
    case 1: return "S (sleeping)";
    case 2: return "Z (zombie)";
    case 3: return "T (stopped)";
    default: return "? (unknown)";
    }
}

static uint32_t
gen_status(char *buf, uint32_t bufsz, aegis_process_t *proc)
{
    char *p = buf;
    char *end = buf + bufsz - 1;

    p = pfs_strcpy(p, "Name:\t");
    if (proc->exe_path[0]) {
        /* Extract basename */
        const char *name = proc->exe_path;
        const char *s = proc->exe_path;
        while (*s) { if (*s == '/') name = s + 1; s++; }
        p = pfs_strcpy(p, name);
    } else {
        p = pfs_strcpy(p, "init");
    }
    *p++ = '\n';

    p = pfs_strcpy(p, "State:\t");
    p = pfs_strcpy(p, task_state_str(proc->task.state));
    *p++ = '\n';

    p = pfs_strcpy(p, "Tgid:\t");
    p = pfs_u64_dec(p, proc->tgid);
    *p++ = '\n';

    p = pfs_strcpy(p, "Pid:\t");
    p = pfs_u64_dec(p, proc->pid);
    *p++ = '\n';

    p = pfs_strcpy(p, "PPid:\t");
    p = pfs_u64_dec(p, proc->ppid);
    *p++ = '\n';

    p = pfs_strcpy(p, "Uid:\t");
    p = pfs_u64_dec(p, proc->uid);
    *p++ = '\n';

    p = pfs_strcpy(p, "Gid:\t");
    p = pfs_u64_dec(p, proc->gid);
    *p++ = '\n';

    /* VmSize — sum of all VMA lengths in kB */
    if (proc->vma_table && p < end - 40) {
        uint64_t vm_size = 0;
        uint32_t vi;
        for (vi = 0; vi < proc->vma_count; vi++)
            vm_size += proc->vma_table[vi].len;
        p = pfs_strcpy(p, "VmSize:\t");
        p = pfs_u64_dec(p, vm_size / 1024);
        p = pfs_strcpy(p, " kB\n");
    }

    (void)end;
    *p = '\0';
    return (uint32_t)(p - buf);
}

static uint32_t
gen_stat(char *buf, uint32_t bufsz, aegis_process_t *proc)
{
    char *p = buf;
    (void)bufsz;

    /* Format: pid (comm) state ppid pgid session tty tgid flags ... */
    p = pfs_u64_dec(p, proc->pid);
    *p++ = ' ';
    *p++ = '(';
    if (proc->exe_path[0]) {
        const char *name = proc->exe_path;
        const char *s = proc->exe_path;
        while (*s) { if (*s == '/') name = s + 1; s++; }
        p = pfs_strcpy(p, name);
    } else {
        p = pfs_strcpy(p, "init");
    }
    *p++ = ')';
    *p++ = ' ';

    /* State char */
    switch (proc->task.state) {
    case 0: *p++ = 'R'; break;
    case 1: *p++ = 'S'; break;
    case 2: *p++ = 'Z'; break;
    case 3: *p++ = 'T'; break;
    default: *p++ = '?'; break;
    }
    *p++ = ' ';

    p = pfs_u64_dec(p, proc->ppid); *p++ = ' ';
    p = pfs_u64_dec(p, proc->pgid); *p++ = ' ';
    p = pfs_u64_dec(p, 0); *p++ = ' ';  /* session */
    p = pfs_u64_dec(p, 0); *p++ = ' ';  /* tty */
    p = pfs_u64_dec(p, proc->tgid); *p++ = ' ';
    p = pfs_u64_dec(p, 0); *p++ = ' ';  /* flags */
    /* Pad remaining fields with 0 (Linux /proc/[pid]/stat has ~52 fields) */
    {
        int fi;
        for (fi = 0; fi < 43; fi++) {
            *p++ = '0';
            *p++ = (fi < 42) ? ' ' : '\n';
        }
    }

    *p = '\0';
    return (uint32_t)(p - buf);
}

static uint32_t
gen_exe(char *buf, uint32_t bufsz, aegis_process_t *proc)
{
    char *p = buf;
    (void)bufsz;
    if (proc->exe_path[0])
        p = pfs_strcpy(p, proc->exe_path);
    *p++ = '\n';
    *p = '\0';
    return (uint32_t)(p - buf);
}

static uint32_t
gen_cmdline(char *buf, uint32_t bufsz, aegis_process_t *proc)
{
    (void)bufsz;
    uint32_t len = 0;
    if (proc->exe_path[0]) {
        const char *s = proc->exe_path;
        while (*s) buf[len++] = *s++;
    }
    buf[len++] = '\0';  /* NUL-terminated as per Linux spec */
    return len;
}

static uint32_t
gen_meminfo(char *buf, uint32_t bufsz)
{
    char *p = buf;
    (void)bufsz;

    uint64_t total_kb = pmm_total_pages() * 4;
    uint64_t free_kb  = pmm_free_pages() * 4;

    p = pfs_strcpy(p, "MemTotal:       ");
    p = pfs_u64_dec(p, total_kb);
    p = pfs_strcpy(p, " kB\n");

    p = pfs_strcpy(p, "MemFree:        ");
    p = pfs_u64_dec(p, free_kb);
    p = pfs_strcpy(p, " kB\n");

    p = pfs_strcpy(p, "MemAvailable:   ");
    p = pfs_u64_dec(p, free_kb);
    p = pfs_strcpy(p, " kB\n");

    *p = '\0';
    return (uint32_t)(p - buf);
}

static uint32_t
gen_version(char *buf, uint32_t bufsz)
{
    (void)bufsz;
    char *p = buf;
    p = pfs_strcpy(p, "Aegis 0.31.0\n");
    *p = '\0';
    return (uint32_t)(p - buf);
}

/* ── procfs VFS ops ──────────────────────────────────────────────────── */

/* priv for regular procfs files: kva-allocated buffer with content */
typedef struct {
    char    *buf;       /* kva-allocated page with generated content */
    uint32_t len;       /* bytes of content */
    uint32_t _pad;
} procfs_file_priv_t;

static int
procfs_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
    procfs_file_priv_t *fp = (procfs_file_priv_t *)priv;
    if (off >= fp->len) return 0;  /* EOF */
    uint64_t avail = fp->len - off;
    if (len > avail) len = avail;
    __builtin_memcpy(buf, fp->buf + off, len);
    return (int)len;
}

static void
procfs_close_fn(void *priv)
{
    procfs_file_priv_t *fp = (procfs_file_priv_t *)priv;
    if (fp->buf)
        kva_free_pages(fp->buf, 1);
    kva_free_pages(fp, 1);
}

static int
procfs_stat_fn(void *priv, k_stat_t *st)
{
    procfs_file_priv_t *fp = (procfs_file_priv_t *)priv;
    __builtin_memset(st, 0, sizeof(*st));
    st->st_dev     = 5;
    st->st_ino     = 1;
    st->st_nlink   = 1;
    st->st_mode    = S_IFREG | 0444;
    st->st_size    = (int64_t)fp->len;
    st->st_blksize = 4096;
    return 0;
}

static const vfs_ops_t s_procfs_file_ops = {
    .read    = procfs_read_fn,
    .write   = (void *)0,
    .close   = procfs_close_fn,
    .readdir = (void *)0,
    .dup     = (void *)0,
    .stat    = procfs_stat_fn,
};

/* ── procfs directory ops ────────────────────────────────────────────── */

/* priv for /proc/[pid]/ directory listing */
typedef struct {
    uint32_t pid;    /* 0 = root /proc/ dir */
    uint8_t  is_fd;  /* 1 = /proc/[pid]/fd/ */
    uint8_t  _pad[3];
} procfs_dir_priv_t;

static int
procfs_dir_readdir_fn(void *priv, uint64_t index,
                      char *name_out, uint8_t *type_out)
{
    procfs_dir_priv_t *dp = (procfs_dir_priv_t *)priv;

    if (dp->is_fd) {
        /* /proc/[pid]/fd/ — list open fd numbers */
        aegis_process_t *proc = proc_find_by_pid(dp->pid);
        if (!proc) return -1;
        uint64_t found = 0;
        uint32_t fi;
        for (fi = 0; fi < PROC_MAX_FDS; fi++) {
            if (proc->fd_table->fds[fi].ops) {
                if (found == index) {
                    /* Convert fd number to string */
                    char tmp[12];
                    char *p = pfs_u64_dec(tmp, fi);
                    *p = '\0';
                    char *d = name_out;
                    char *s = tmp;
                    while (*s) *d++ = *s++;
                    *d = '\0';
                    *type_out = 8;  /* DT_REG */
                    return 0;
                }
                found++;
            }
        }
        return -1;
    }

    if (dp->pid != 0) {
        /* /proc/[pid]/ — fixed entries */
        static const char *entries[] = {
            "maps", "status", "stat", "exe", "cmdline", "fd"
        };
        static const uint8_t types[] = {
            8, 8, 8, 8, 8, 4  /* fd is a directory */
        };
        if (index < 6) {
            char *d = name_out;
            const char *s = entries[index];
            while (*s) *d++ = *s++;
            *d = '\0';
            *type_out = types[index];
            return 0;
        }
        return -1;
    }

    /* Root /proc/ directory */
    /* Fixed entries: self, meminfo, version */
    if (index == 0) {
        char *d = name_out; const char *s = "self";
        while (*s) *d++ = *s++; *d = '\0';
        *type_out = 4;  /* DT_DIR */
        return 0;
    }
    if (index == 1) {
        char *d = name_out; const char *s = "meminfo";
        while (*s) *d++ = *s++; *d = '\0';
        *type_out = 8;
        return 0;
    }
    if (index == 2) {
        char *d = name_out; const char *s = "version";
        while (*s) *d++ = *s++; *d = '\0';
        *type_out = 8;
        return 0;
    }

    /* Live process PIDs */
    uint64_t pid_idx = index - 3;
    aegis_task_t *cur = sched_current();
    if (!cur) return -1;
    aegis_task_t *t = cur;
    uint64_t found = 0;
    do {
        if (t->is_user) {
            aegis_process_t *p = (aegis_process_t *)t;
            if (found == pid_idx) {
                char tmp[12];
                char *pp = pfs_u64_dec(tmp, p->pid);
                *pp = '\0';
                char *d = name_out;
                char *s = tmp;
                while (*s) *d++ = *s++;
                *d = '\0';
                *type_out = 4;  /* DT_DIR */
                return 0;
            }
            found++;
        }
        t = t->next;
    } while (t != cur);

    return -1;
}

static void
procfs_dir_close_fn(void *priv)
{
    kva_free_pages(priv, 1);
}

static int
procfs_dir_stat_fn(void *priv, k_stat_t *st)
{
    (void)priv;
    __builtin_memset(st, 0, sizeof(*st));
    st->st_dev   = 5;
    st->st_ino   = 1;
    st->st_nlink = 2;
    st->st_mode  = S_IFDIR | 0555;
    return 0;
}

static const vfs_ops_t s_procfs_dir_ops = {
    .read    = (void *)0,
    .write   = (void *)0,
    .close   = procfs_dir_close_fn,
    .readdir = procfs_dir_readdir_fn,
    .dup     = (void *)0,
    .stat    = procfs_dir_stat_fn,
};

/* ── capability check helper ─────────────────────────────────────────── */

static int
procfs_check_access(uint32_t target_pid)
{
    aegis_task_t *cur = sched_current();
    if (!cur || !cur->is_user) return -1;
    aegis_process_t *caller = (aegis_process_t *)cur;

    /* Self access always allowed */
    if (target_pid == caller->pid)
        return 0;

    /* Other-pid access requires CAP_KIND_PROC_READ */
    return cap_check(caller->caps, CAP_TABLE_SIZE,
                     CAP_KIND_PROC_READ, CAP_RIGHTS_READ);
}

/* ── procfs_open (main dispatch) ─────────────────────────────────────── */

void
procfs_init(void)
{
    /* No global state to initialize. */
}

/* Open a directory listing for /proc/, /proc/[pid]/, or /proc/[pid]/fd/ */
static int
procfs_open_dir(uint32_t pid, int is_fd, vfs_file_t *out)
{
    procfs_dir_priv_t *dp = kva_alloc_pages(1);
    if (!dp) return -12;
    dp->pid   = pid;
    dp->is_fd = (uint8_t)is_fd;
    dp->_pad[0] = dp->_pad[1] = dp->_pad[2] = 0;
    out->ops    = &s_procfs_dir_ops;
    out->priv   = dp;
    out->offset = 0;
    out->size   = 0;
    out->flags  = 0;
    out->_pad   = 0;
    return 0;
}

/* Open a regular procfs file with generated content */
static int
procfs_open_generated(uint32_t (*gen_fn)(char *, uint32_t, aegis_process_t *),
                      aegis_process_t *proc, vfs_file_t *out)
{
    procfs_file_priv_t *fp = kva_alloc_pages(1);
    if (!fp) return -12;
    fp->buf = kva_alloc_pages(1);
    if (!fp->buf) {
        kva_free_pages(fp, 1);
        return -12;
    }
    fp->len = gen_fn(fp->buf, 4096, proc);
    out->ops    = &s_procfs_file_ops;
    out->priv   = fp;
    out->offset = 0;
    out->size   = (uint64_t)fp->len;
    out->flags  = 0;
    out->_pad   = 0;
    return 0;
}

/* Variant for global files (no process arg) */
static int
procfs_open_global(uint32_t (*gen_fn)(char *, uint32_t), vfs_file_t *out)
{
    procfs_file_priv_t *fp = kva_alloc_pages(1);
    if (!fp) return -12;
    fp->buf = kva_alloc_pages(1);
    if (!fp->buf) {
        kva_free_pages(fp, 1);
        return -12;
    }
    fp->len = gen_fn(fp->buf, 4096);
    out->ops    = &s_procfs_file_ops;
    out->priv   = fp;
    out->offset = 0;
    out->size   = (uint64_t)fp->len;
    out->flags  = 0;
    out->_pad   = 0;
    return 0;
}

int
procfs_open(const char *path, int flags, vfs_file_t *out)
{
    (void)flags;

    /* path is relative to /proc/ (e.g., "self/maps", "123/status", "meminfo") */

    /* Root /proc/ directory (empty path or just "/") */
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0'))
        return procfs_open_dir(0, 0, out);

    /* Global files */
    if (pfs_streq(path, "meminfo"))
        return procfs_open_global(gen_meminfo, out);
    if (pfs_streq(path, "version"))
        return procfs_open_global(gen_version, out);

    /* Parse pid or "self" */
    uint32_t pid;
    const char *rest;

    if (path[0] == 's' && path[1] == 'e' && path[2] == 'l' && path[3] == 'f') {
        aegis_task_t *cur = sched_current();
        if (!cur || !cur->is_user) return -2;
        pid = ((aegis_process_t *)cur)->pid;
        rest = path + 4;
    } else {
        pid = parse_pid_str(path, &rest);
        if (pid == 0) return -2;
    }

    /* Check access for non-self pids */
    if (procfs_check_access(pid) != 0)
        return (int)-(int64_t)ENOCAP;

    /* Look up process */
    aegis_process_t *proc = proc_find_by_pid(pid);
    if (!proc) return -2;

    /* /proc/[pid]/ directory */
    if (rest[0] == '\0' || (rest[0] == '/' && rest[1] == '\0'))
        return procfs_open_dir(pid, 0, out);

    /* Skip leading slash */
    if (rest[0] == '/') rest++;

    /* Dispatch to file type */
    if (pfs_streq(rest, "maps"))
        return procfs_open_generated(gen_maps, proc, out);
    if (pfs_streq(rest, "status"))
        return procfs_open_generated(gen_status, proc, out);
    if (pfs_streq(rest, "stat"))
        return procfs_open_generated(gen_stat, proc, out);
    if (pfs_streq(rest, "exe"))
        return procfs_open_generated(gen_exe, proc, out);
    if (pfs_streq(rest, "cmdline"))
        return procfs_open_generated(gen_cmdline, proc, out);

    /* /proc/[pid]/fd or /proc/[pid]/fd/ */
    if (rest[0] == 'f' && rest[1] == 'd' && (rest[2] == '\0' || rest[2] == '/'))
        return procfs_open_dir(pid, 1, out);

    return -2;  /* ENOENT */
}

int
procfs_stat(const char *path, k_stat_t *out)
{
    if (!path || !out) return -2;

    __builtin_memset(out, 0, sizeof(*out));
    out->st_dev   = 5;
    out->st_nlink = 1;

    /* /proc itself */
    if (pfs_streq(path, "/proc")) {
        out->st_ino   = 1;
        out->st_nlink = 2;
        out->st_mode  = S_IFDIR | 0555;
        return 0;
    }

    /* /proc/ prefix */
    if (path[0] != '/' || path[1] != 'p' || path[2] != 'r' ||
        path[3] != 'o' || path[4] != 'c') return -2;

    const char *rel = path + 5;
    if (rel[0] == '/') rel++;
    if (rel[0] == '\0') {
        out->st_ino  = 1;
        out->st_nlink = 2;
        out->st_mode = S_IFDIR | 0555;
        return 0;
    }

    /* Global files */
    if (pfs_streq(rel, "meminfo") || pfs_streq(rel, "version")) {
        out->st_ino  = 2;
        out->st_mode = S_IFREG | 0444;
        return 0;
    }

    /* /proc/self or /proc/[pid] — directories */
    if (pfs_streq(rel, "self")) {
        out->st_ino  = 3;
        out->st_nlink = 2;
        out->st_mode = S_IFDIR | 0555;
        return 0;
    }

    /* Numeric pid directory */
    const char *after;
    uint32_t pid = parse_pid_str(rel, &after);
    if (pid > 0 && (*after == '\0' || *after == '/')) {
        if (*after == '\0') {
            /* /proc/[pid] directory */
            out->st_ino  = (uint64_t)pid;
            out->st_nlink = 2;
            out->st_mode = S_IFDIR | 0555;
            return 0;
        }
        /* /proc/[pid]/... file */
        after++;  /* skip '/' */
        if (pfs_streq(after, "fd") || pfs_streq(after, "fd/")) {
            out->st_ino  = (uint64_t)pid + 100;
            out->st_nlink = 2;
            out->st_mode = S_IFDIR | 0555;
            return 0;
        }
        /* Regular proc files */
        if (pfs_streq(after, "maps") || pfs_streq(after, "status") ||
            pfs_streq(after, "stat") || pfs_streq(after, "exe") ||
            pfs_streq(after, "cmdline")) {
            out->st_ino  = (uint64_t)pid + 200;
            out->st_mode = S_IFREG | 0444;
            return 0;
        }
    }

    return -2;
}
```

- [ ] **Step 3: Add /proc/ dispatch to vfs.c**

In `kernel/fs/vfs.c`, add include at top:
```c
#include "procfs.h"
```

In `vfs_init()`, after the `initrd_register()` call (line ~32), add:
```c
    procfs_init();
```

In `vfs_open()`, add before the `/* 1. /run/ → run ramfs */` comment (around line 211):
```c
    /* /proc/ → procfs */
    if (path[0]=='/' && path[1]=='p' && path[2]=='r' && path[3]=='o' &&
        path[4]=='c' && (path[5]=='/' || path[5]=='\0'))
        return procfs_open(path + 5 + (path[5]=='/' ? 1 : 0), flags, out);
```

In `vfs_stat_path()`, add before the `/* /run/ → run ramfs */` section (around line 336). First add `/proc` to the directory path check:

Change:
```c
    if (streq(path, "/")    || streq(path, "/etc")  || streq(path, "/bin") ||
        streq(path, "/dev") || streq(path, "/root") || streq(path, "/run")) {
```
to:
```c
    if (streq(path, "/")    || streq(path, "/etc")  || streq(path, "/bin") ||
        streq(path, "/dev") || streq(path, "/root") || streq(path, "/run") ||
        streq(path, "/proc")) {
```

Then add a procfs stat dispatch. After the directory paths block but before the `/dev/` block (around line 301), add:
```c
    /* /proc/* → procfs stat */
    if (path[0]=='/' && path[1]=='p' && path[2]=='r' && path[3]=='o' &&
        path[4]=='c' && (path[5]=='/' || path[5]=='\0'))
        return procfs_stat(path, out);
```

- [ ] **Step 4: Add procfs.c to FS_SRCS in Makefile**

Change:
```
FS_SRCS = \
    kernel/fs/fd_table.c \
    kernel/fs/vfs.c \
    ...
    kernel/fs/ramfs.c \
```
to:
```
FS_SRCS = \
    kernel/fs/fd_table.c \
    kernel/fs/vfs.c \
    ...
    kernel/fs/ramfs.c \
    kernel/fs/procfs.c
```

(Add `kernel/fs/procfs.c` at the end of FS_SRCS, adding a `\` after `ramfs.c`)

- [ ] **Step 5: Build to verify**

Run: `rm -rf build && make`
Expected: Clean build.

- [ ] **Step 6: Commit**

```bash
git add kernel/fs/procfs.h kernel/fs/procfs.c kernel/fs/vfs.c Makefile
git commit -m "feat: add /proc virtual filesystem with capability-gated access"
```

---

### Task 7: User Test Binary + Integration Test

**Files:**
- Create: `user/proc_test/main.c`
- Create: `user/proc_test/Makefile`
- Create: `tests/test_proc.py`
- Modify: `tests/run_tests.sh` (add test_proc.py)
- Modify: `Makefile` (add proc_test to DISK_USER_BINS and disk build)

- [ ] **Step 1: Create user/proc_test/Makefile**

```makefile
CC      = musl-gcc
CFLAGS  = -static -O2 -fno-pie -no-pie -Wl,--build-id=none
TARGET  = proc_test.elf

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
```

- [ ] **Step 2: Create user/proc_test/main.c**

```c
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>

static int test_maps(void)
{
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) {
        printf("PROC FAIL: open /proc/self/maps\n");
        return 1;
    }
    char buf[2048];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        printf("PROC FAIL: read /proc/self/maps empty\n");
        return 1;
    }
    buf[n] = '\0';
    if (!strstr(buf, "[stack]")) {
        printf("PROC FAIL: /proc/self/maps missing [stack]\n");
        printf("  got: %s\n", buf);
        return 1;
    }
    return 0;
}

static int test_status(void)
{
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0) {
        printf("PROC FAIL: open /proc/self/status\n");
        return 1;
    }
    char buf[1024];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        printf("PROC FAIL: read /proc/self/status empty\n");
        return 1;
    }
    buf[n] = '\0';

    /* Verify Pid: line exists and matches getpid() */
    pid_t pid = getpid();
    char needle[32];
    snprintf(needle, sizeof(needle), "Pid:\t%d\n", (int)pid);
    if (!strstr(buf, needle)) {
        printf("PROC FAIL: /proc/self/status missing '%s'\n", needle);
        printf("  got: %s\n", buf);
        return 1;
    }
    return 0;
}

static int test_meminfo(void)
{
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) {
        printf("PROC FAIL: open /proc/meminfo\n");
        return 1;
    }
    char buf[512];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        printf("PROC FAIL: read /proc/meminfo empty\n");
        return 1;
    }
    buf[n] = '\0';
    if (!strstr(buf, "MemTotal:")) {
        printf("PROC FAIL: /proc/meminfo missing MemTotal\n");
        return 1;
    }
    return 0;
}

static int test_stat(void)
{
    int fd = open("/proc/self/stat", O_RDONLY);
    if (fd < 0) {
        printf("PROC FAIL: open /proc/self/stat\n");
        return 1;
    }
    char buf[512];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        printf("PROC FAIL: read /proc/self/stat empty\n");
        return 1;
    }
    buf[n] = '\0';

    /* First field should be our pid */
    pid_t pid = getpid();
    int got_pid = 0;
    sscanf(buf, "%d", &got_pid);
    if (got_pid != (int)pid) {
        printf("PROC FAIL: /proc/self/stat pid=%d expected=%d\n",
               got_pid, (int)pid);
        return 1;
    }
    return 0;
}

static int test_fd_dir(void)
{
    /* Use getdents64 via opendir/readdir */
    DIR *d = opendir("/proc/self/fd");
    if (!d) {
        printf("PROC FAIL: opendir /proc/self/fd\n");
        return 1;
    }
    int found_0 = 0, found_1 = 0, found_2 = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, "0") == 0) found_0 = 1;
        if (strcmp(de->d_name, "1") == 0) found_1 = 1;
        if (strcmp(de->d_name, "2") == 0) found_2 = 1;
    }
    closedir(d);
    if (!found_0 || !found_1 || !found_2) {
        printf("PROC FAIL: /proc/self/fd missing stdin/stdout/stderr (%d%d%d)\n",
               found_0, found_1, found_2);
        return 1;
    }
    return 0;
}

int main(void)
{
    if (test_maps()) return 1;
    if (test_status()) return 1;
    if (test_meminfo()) return 1;
    if (test_stat()) return 1;
    if (test_fd_dir()) return 1;
    printf("PROC OK\n");
    return 0;
}
```

- [ ] **Step 3: Create tests/test_proc.py**

```python
#!/usr/bin/env python3
"""test_proc.py — Phase 31 /proc filesystem smoke test.

Boots Aegis with q35 + NVMe disk (ext2 with proc_test binary),
logs in, runs /bin/proc_test, checks for "PROC OK" in output.

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

        # Run proc test
        time.sleep(1)
        _type_string(mon, "/bin/proc_test\n")

        if serial.wait_for("PROC OK", time.time() + CMD_TIMEOUT):
            print("PASS: proc_test reported PROC OK")
        else:
            print("FAIL: 'PROC OK' not found in output")
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

- [ ] **Step 4: Add proc_test to Makefile**

Add build rule after the `mmap_test` rule (around line 303):
```makefile
user/proc_test/proc_test.elf: user/proc_test/main.c
	$(MAKE) -C user/proc_test
```

Add to DISK_USER_BINS (after `user/mmap_test/mmap_test.elf \`):
```
	user/proc_test/proc_test.elf \
```

In the `debugfs` commands that write binaries to the ext2 disk, add after the mmap_test write:
```
write user/proc_test/proc_test.elf /bin/proc_test\n
```

This goes in the long `printf` pipeline that feeds `debugfs`.

- [ ] **Step 5: Add test_proc.py to run_tests.sh**

In `tests/run_tests.sh`, after the `test_mmap` section, add:
```bash
echo "--- test_proc ---"
python3 tests/test_proc.py
```

- [ ] **Step 6: Build disk and run test**

Run:
```bash
rm -rf build && make INIT=vigil iso && make disk
python3 tests/test_proc.py
```
Expected: `PASS: proc_test reported PROC OK`

- [ ] **Step 7: Run full test suite**

Run: `make test`
Expected: PASS (boot.txt matches with 9 capabilities).

- [ ] **Step 8: Commit**

```bash
git add user/proc_test/main.c user/proc_test/Makefile \
        tests/test_proc.py tests/run_tests.sh Makefile
git commit -m "feat: add proc_test binary and test_proc.py integration test"
```

---

### Task 8: Update CLAUDE.md

**Files:**
- Modify: `.claude/CLAUDE.md` (build status, forward constraints, roadmap)

- [ ] **Step 1: Update build status table**

Add row after "mprotect + mmap freelist":
```
| /proc filesystem (Phase 31) | ✅ | VMA tracking; procfs VFS; /proc/self/maps,status,stat,exe,cmdline,fd; /proc/meminfo; CAP_KIND_PROC_READ; test_proc.py PASS |
```

- [ ] **Step 2: Update phase roadmap**

Change Phase 31 status from "Not started" to "✅ Done".

- [ ] **Step 3: Add Phase 31 forward constraints section**

```markdown
## Phase 31 — Forward Constraints

**Phase 31 status: ✅ complete. `make test` passes. `test_proc.py` PASS.**

1. **VMA table has no spinlock.** Safe single-core (syscalls non-preemptible). SMP requires a per-table spinlock.

2. **VMA refcount for CLONE_VM is not locked.** The refcount increment/decrement is safe single-core. SMP needs atomic ops.

3. **`/proc/[pid]/exe` is a plain text file, not a symlink.** Phase 38 (symlinks) can upgrade it.

4. **cmdline stores exe name only, not full argv.** Full argv tracking deferred.

5. **`/proc/[pid]/fd/` entries are plain names, not symlinks.** No target path info.

6. **init's ELF segment VMAs are not tracked.** `proc_spawn` calls `elf_load` before the process is on the run queue, so `sched_current()` returns the idle task. ELF VMAs for init are missing from `/proc/1/maps`. All exec'd processes have correct VMA tracking.

7. **`pmm_free_pages()` scans the entire bitmap.** O(128KB / 8 = 16K iterations) for 128MB. Fast enough. For multi-GB memory, add a running counter.

8. **Procfs allocates 2 kva pages per open (priv + buffer).** Each `/proc` file open costs 8KB of kva. Close frees both. No caching — acceptable for diagnostic tools that open, read, close.
```

- [ ] **Step 4: Update last-updated timestamp**

```
*Last updated: 2026-03-27 — Phase 31 complete. /proc filesystem ✅. VMA tracking ✅. test_proc.py PASS.*
```

- [ ] **Step 5: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs: update CLAUDE.md for Phase 31 /proc filesystem completion"
```
