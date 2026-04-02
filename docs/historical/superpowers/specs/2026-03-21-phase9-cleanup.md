# Phase 9: Syscall Infrastructure Cleanup + Process Memory Reclaim Design

## Goal

Three related cleanup items accumulated across Phases 5–8:

1. **`USER_ADDR_MAX` migration** — move `USER_ADDR_MAX` and `user_ptr_valid` out of
   `syscall.c` into a shared header so future syscall handlers don't duplicate them.
2. **`copy_from_user`** — replace the per-byte `stac`/`clac` loop in `sys_write` with a
   single `stac`→`memcpy`→`clac` pattern, centralising user-memory access in one place.
3. **kva free path + `sched_exit` cleanup** — add `vmm_unmap_page` and `kva_free_pages`,
   then wire them into `sched_exit` so kernel objects (TCB, kernel stack, PCB) are
   returned to the PMM when a process exits instead of leaking permanently.

No new syscalls are added. `sys_read` is deferred to Phase 10 once there is something
to read from (VFS).

---

## Background

### Why the migration matters

`USER_ADDR_MAX` is currently `#define`d at the top of `kernel/syscall/syscall.c`. When
`sys_read` or any other pointer-taking syscall is added, copy-pasting the constant is a
vulnerability — a divergence would silently accept addresses the other side rejects.
Moving it to a shared header eliminates the duplication at the source.

### Why copy_from_user matters

Phase 8 used a per-character `stac`/`clac` window: one pair of privileged instructions
per byte. On a 4 KB write that is 8192 ring transitions through the AC bit. The correct
pattern is one `stac`, one `memcpy`, one `clac` per call — regardless of length. Phase 9
introduces that pattern. The per-character loop in `sys_write` is replaced with a
chunked copy through a 256-byte stack buffer.

### Why the free path matters

`kva_alloc_pages` is a one-way bump allocator. Every `proc_spawn` call allocates:
- 1 kva page for `aegis_process_t` (PCB, contains the embedded `aegis_task_t`)
- 1 kva page for the kernel stack

Neither is ever returned to the PMM. Each user process exit permanently consumes 2
physical pages (8 KB). In a long-running kernel this exhausts physical memory.

### Why deferred cleanup is required

`sched_exit` cannot free the dying task's TCB or stack immediately before `ctx_switch`:

- `ctx_switch` writes `dying->rsp` into the TCB — the TCB must be valid until after
  the RSP switch.
- The dying task's kernel stack is the current stack until `ctx_switch` switches RSP to
  `s_current->rsp`.

The fix is a deferred pattern: record the dying task's pointers in static globals before
`ctx_switch`, then free them at the *entry* of the next `sched_exit` call. The final
exited process's resources are released at the start of the subsequent exit (if any) or
leaked at shutdown — acceptable for Phase 9 scope.

---

## Architecture

### New file: `kernel/syscall/syscall_util.h`

Validation helpers shared across all syscall handlers:

```c
#ifndef SYSCALL_UTIL_H
#define SYSCALL_UTIL_H

#include <stdint.h>

/* USER_ADDR_MAX — highest canonical user-space virtual address (x86-64).
 * Shared by all syscall handlers. Do not redefine in individual syscall files. */
#define USER_ADDR_MAX 0x00007FFFFFFFFFFFUL

/* user_ptr_valid — return 1 if [addr, addr+len) lies entirely within the
 * canonical user address space, 0 otherwise.
 * For len=0, validates that addr itself is a canonical user address (does
 * NOT unconditionally pass — a kernel addr with len=0 still returns 0).
 * Overflow-safe: addr <= USER_ADDR_MAX - len avoids addr+len wraparound. */
static inline int
user_ptr_valid(uint64_t addr, uint64_t len)
{
    return len <= USER_ADDR_MAX && addr <= USER_ADDR_MAX - len;
}

#endif /* SYSCALL_UTIL_H */
```

### New file: `kernel/mm/uaccess.h`

User-memory transfer helpers. Separated from `syscall_util.h` because transfer is a
memory operation while validation is a bounds-check operation — they will be included
by different consumers as the kernel grows.

```c
#ifndef UACCESS_H
#define UACCESS_H

#include "arch.h"
#include <stdint.h>

/* copy_from_user — copy len bytes from user-space src to kernel-space dst.
 *
 * Caller MUST validate [src, src+len) with user_ptr_valid() before calling.
 * Uses a single arch_stac/arch_clac window around the entire copy — one pair
 * of AC-bit transitions regardless of len, replacing the per-byte pattern.
 *
 * The "memory" clobbers inside arch_stac/arch_clac prevent the compiler from
 * hoisting the memcpy before stac or sinking it past clac.
 *
 * No fault recovery: GCC may vectorize __builtin_memcpy, emitting multi-byte
 * loads. If [src, src+len) crosses a page boundary where the second page is
 * unmapped, a #PF fires with AC=1. There is no fixup table (Linux extable).
 * Caller must ensure the entire range is mapped before calling. */
static inline void
copy_from_user(void *dst, const void *src, uint64_t len)
{
    arch_stac();
    __builtin_memcpy(dst, src, len);
    arch_clac();
}

#endif /* UACCESS_H */
```

### Changes to `kernel/syscall/syscall.c`

Remove the local `#define USER_ADDR_MAX` and `user_ptr_valid` definition. Add:

```c
#include "syscall_util.h"
#include "uaccess.h"
```

Rewrite `sys_write` to use chunked `copy_from_user`:

```c
static uint64_t
sys_write(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    (void)arg1;
    if (!user_ptr_valid(arg2, arg3))
        return (uint64_t)-14;   /* EFAULT */
    const char *s = (const char *)(uintptr_t)arg2;
    char kbuf[256];
    uint64_t offset = 0;
    while (offset < arg3) {
        uint64_t n = arg3 - offset;
        if (n > sizeof(kbuf)) n = sizeof(kbuf);
        copy_from_user(kbuf, s + offset, n);
        uint64_t j;
        for (j = 0; j < n; j++)
            printk("%c", kbuf[j]);
        offset += n;
    }
    return arg3;
}
```

One `stac`/`clac` pair per 256-byte chunk instead of one per byte. `printk` is called
outside the AC window.

### `kernel/mm/vmm.c` + `kernel/mm/vmm.h` — no change needed

**`vmm_unmap_page(uint64_t virt)`** was already implemented and declared as part of
Phase 8 work. It performs a 4-level walk using the window pattern (identical to
`vmm_phys_of`), clears the PTE, and calls `arch_vmm_invlpg`. Panics if any level is
not present. **Do not re-implement; it is already present.**

### Changes to `kernel/mm/kva.c` + `kernel/mm/kva.h`

**`kva_free_pages(void *va, uint64_t n)`** — for each of the `n` pages starting at `va`:

Cast `va` to `uint64_t` once before the loop (pointer arithmetic on `void *` is
undefined in standard C and a compile error under `-Werror`):

```c
uint64_t addr = (uint64_t)(uintptr_t)va;
```

Then for `i` from 0 to `n-1`:

1. Recover the physical address via `vmm_phys_of(addr + i * 4096UL)`
2. Unmap the virtual address via `vmm_unmap_page(addr + i * 4096UL)`
3. Return the physical frame to the PMM via `pmm_free_page(phys)`

The virtual address is permanently abandoned (not returned to the bump cursor). VA space
is not the scarce resource; physical pages are.

Declaration in `kva.h`:
```c
void kva_free_pages(void *va, uint64_t n);
```

### Changes to `kernel/sched/sched.c`

Add three static globals above `sched_exit`:

```c
/* Deferred cleanup: dying task's resources cannot be freed before ctx_switch
 * (ctx_switch writes dying->rsp; the dying stack is live until RSP switches).
 * Record them here and free at the entry of the next sched_exit call. */
static void    *g_prev_dying_tcb         = NULL;
static void    *g_prev_dying_stack       = NULL;
static uint64_t g_prev_dying_stack_pages = 0;
```

At the start of `sched_exit`, before any list manipulation:

```c
if (g_prev_dying_tcb) {
    kva_free_pages(g_prev_dying_stack, g_prev_dying_stack_pages);
    kva_free_pages(g_prev_dying_tcb, 1);
    g_prev_dying_tcb = NULL;
}
```

Just before `ctx_switch`, after all list manipulation is complete:

```c
g_prev_dying_stack       = (void *)dying->stack_base;
g_prev_dying_stack_pages = dying->is_user ? 1 : STACK_PAGES;
g_prev_dying_tcb         = dying;
ctx_switch(dying, s_current);
__builtin_unreachable();
```

**Why `is_user ? 1 : STACK_PAGES`:** user processes (spawned via `proc_spawn`) allocate
1 kva page for the kernel stack; kernel tasks (spawned via `sched_spawn`) allocate
`STACK_PAGES` (4) kva pages. The `is_user` flag encodes which allocation size was used.

**Why `kva_free_pages(dying, 1)` frees the PCB:** for user processes, `dying` points to
`&proc->task` which is the first field of `aegis_process_t` at offset 0. Since `task`
is at offset 0, `dying == (aegis_task_t *)proc`, so freeing the page at `dying` frees
the entire PCB. For kernel tasks, `dying` points to the `aegis_task_t` directly.

---

## Files Changed

| File | Change |
|------|--------|
| `kernel/syscall/syscall_util.h` | New — `USER_ADDR_MAX` + `user_ptr_valid` |
| `kernel/mm/uaccess.h` | New — `copy_from_user` static inline |
| `kernel/syscall/syscall.c` | Add includes; remove local definitions; rewrite `sys_write` |
| `kernel/mm/vmm.c` | No change — `vmm_unmap_page` already implemented (Phase 8) |
| `kernel/mm/vmm.h` | No change — `vmm_unmap_page` already declared (Phase 8) |
| `kernel/mm/kva.c` | Add `kva_free_pages` |
| `kernel/mm/kva.h` | Declare `kva_free_pages` |
| `kernel/sched/sched.c` | Add deferred-free globals; wire cleanup into `sched_exit` |
| `.claude/CLAUDE.md` | Update build status table; add Phase 10 constraints |

---

## Test Oracle

`tests/expected/boot.txt` is **unchanged**. No new `[SUBSYSTEM]` lines are emitted.
`make test` continues to use the Phase 8 oracle. The GREEN criterion is `make test`
exits 0 after all changes. Three audits confirm correctness:

```bash
# USER_ADDR_MAX defined only in syscall_util.h, not in syscall.c
grep -rn 'USER_ADDR_MAX' kernel/

# arch_stac()/arch_clac() call sites only in uaccess.h (not in syscall.c)
grep -rn 'arch_stac()\|arch_clac()' kernel/

# syscall.c has no direct arch_stac/arch_clac references
grep -n 'arch_stac\|arch_clac' kernel/syscall/syscall.c   # must be empty
```

---

## Success Criteria

1. `make test` exits 0.
2. `grep -rn 'USER_ADDR_MAX' kernel/` shows the definition only in
   `kernel/syscall/syscall_util.h` and usage in `kernel/syscall/syscall.c`.
3. `grep -n 'arch_stac\|arch_clac' kernel/syscall/syscall.c` produces no output
   (stac/clac now encapsulated in `copy_from_user` in `uaccess.h`).
4. `kva_free_pages` compiles and links without error.

---

## Phase 10 Forward-Looking Constraints

**`copy_to_user` not yet needed.** When `sys_read` arrives in Phase 10, a symmetric
`copy_to_user(dst_user, src_kernel, len)` goes in `uaccess.h` alongside
`copy_from_user`. The pattern is identical: `arch_stac`, `memcpy`, `arch_clac`.

**User address space teardown deferred.** `sched_exit` still leaks the user PML4 and
ELF segment pages. Phase 10 must walk the user PML4, free all non-kernel-half mapped
pages, and free the PML4 page itself. This requires a `vmm_walk_user_pml4` helper that
iterates all present PTEs in PML4 entries 0–255 and calls `pmm_free_page` for each.

**`is_user ? 1 : STACK_PAGES` is a hidden contract.** The `g_prev_dying_stack_pages`
calculation reconstructs the stack size from `is_user` rather than storing it
directly. If any kernel task is ever allocated with a stack size other than `STACK_PAGES`,
this silently frees the wrong number of pages. Phase 10 should add a `stack_pages`
field to `aegis_task_t` and populate it at allocation time (`sched_spawn`,
`proc_spawn`), eliminating the inference.

**Last-exit leak.** The deferred cleanup pattern releases the TCB and stack of the
*previous* exited process, not the current one. The simplest fix is not `sys_waitpid`
but a permanent idle task that never exits — if a non-exiting kernel task is always
in the run queue, the "next `sched_exit` frees the previous" pattern means every
exiting process gets cleaned up. Phase 10 should rename `task_heartbeat` to
`task_idle`, remove the 500-tick exit, and add a separate shutdown mechanism.

**User address space teardown deferred.** `sched_exit` still leaks the user PML4 and
ELF segment pages. Phase 10 must walk the user PML4 and free all mapped pages.
`vmm_walk_user_pml4` must only iterate PML4 entries 0–255 (user half), skip any
entry that is zero, and never touch entries 256–511 (kernel half) — those pages are
shared with the master PML4 and freeing them would corrupt every other process.

**`vmm_unmap_page` is single-CPU.** Uses `invlpg` (local TLB invalidation only). When
SMP is introduced, every `vmm_unmap_page` call must be followed by a TLB shootdown IPI
to all other CPUs before the physical page is reused.
