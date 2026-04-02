# Phase 30: mprotect + mmap Improvements — Design Spec

## Goal

Replace the mprotect stub and mmap bump-only allocator with real implementations. mprotect changes page permissions (enabling guard pages and W^X enforcement). munmap returns VA ranges to a per-process freelist so mmap can reuse them (fixing the thread-stack VA leak).

## Architecture

Two independent changes that touch the same subsystem:

1. **vmm_set_user_prot + sys_mprotect** — walks page tables, updates PTE flags in-place, flushes TLB.
2. **Per-process mmap freelist + sys_munmap insertion + sys_mmap freelist-first allocation** — a 256-slot static array of `(base, len)` free regions, coalesced on insert, best-fit on alloc.

Both changes live in `kernel/mm/vmm.c` (new VMM helper) and `kernel/syscall/sys_memory.c` (syscall layer). The freelist struct lives in `kernel/proc/proc.h`.

## Detailed Design

### 1. vmm_set_user_prot

New function in `kernel/mm/vmm.c`:

```c
int vmm_set_user_prot(uint64_t pml4_phys, uint64_t virt, uint64_t flags);
```

- Walks the 4-level page table via the window allocator to reach the leaf PTE.
- If the PTE is not present and flags request PROT_NONE, returns 0 (no-op).
- If the PTE is not present and flags request a real mapping, returns -1 (skip — matching Linux behavior where mprotect silently skips unmapped pages in the range).
- Preserves the physical address in the PTE; overwrites only the flag bits.
- Issues `invlpg` for the modified virtual address.
- Returns 0 on success.

Declared in `kernel/mm/vmm.h`.

### 2. sys_mprotect

Replace the stub in `kernel/syscall/sys_memory.c`:

```c
uint64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot)
```

**Validation:**
- `addr` must be page-aligned (addr & 0xFFF == 0), else -EINVAL.
- `len` rounded up to page boundary.
- `addr + len` must not exceed USER_ADDR_MAX, else -EINVAL.
- `prot` must be a valid combination of PROT_NONE/PROT_READ/PROT_WRITE/PROT_EXEC.

**PROT-to-PTE flag mapping:**

| prot | PTE flags |
|------|-----------|
| PROT_NONE (0) | clear PRESENT — page becomes inaccessible |
| PROT_READ (1) | PRESENT \| USER \| NX |
| PROT_READ \| PROT_WRITE (3) | PRESENT \| USER \| WRITABLE \| NX |
| PROT_READ \| PROT_EXEC (5) | PRESENT \| USER (NX cleared) |
| PROT_READ \| PROT_WRITE \| PROT_EXEC (7) | PRESENT \| USER \| WRITABLE (NX cleared) |
| PROT_WRITE without PROT_READ (2) | treat as PROT_READ \| PROT_WRITE (x86 can't do write-only) |
| PROT_EXEC without PROT_READ (4) | treat as PROT_READ \| PROT_EXEC (x86 can't do exec-only) |

**W^X enforcement:** mmap sets NX by default on all anonymous pages. Only an explicit PROT_EXEC clears NX. The kernel never produces WRITABLE + !NX unless the caller explicitly requests RWX.

**Loop:** For each page in `[addr, addr+len)`, call `vmm_set_user_prot()`. Skip unmapped pages silently (match Linux).

### 3. Per-process mmap freelist

**Data structure** in `kernel/proc/proc.h`:

```c
#define MMAP_FREE_MAX 256

typedef struct {
    uint64_t base;
    uint64_t len;
} mmap_free_t;

/* Inside aegis_process_t: */
    mmap_free_t   mmap_free[MMAP_FREE_MAX];
    uint32_t      mmap_free_count;
```

Total size: 256 × 16 + 4 = 4100 bytes. Fits in the existing 4-page (16KB) PCB allocation with room to spare.

**Operations:**

#### mmap_free_insert(proc, base, len)

1. Check if the new region is contiguous with any existing entry:
   - If `existing.base + existing.len == base`: merge (extend existing.len by len).
   - If `base + len == existing.base`: merge (set existing.base = base, existing.len += len).
   - After one merge, check if the merged entry is now contiguous with another entry (double coalesce).
2. If no merge: append a new entry if `mmap_free_count < MMAP_FREE_MAX`.
3. If freelist is full: drop the VA (graceful degradation — physical pages still freed).

#### mmap_free_alloc(proc, len) → base or 0

1. Walk the freelist for the **best-fit** entry (smallest entry where `entry.len >= len`).
2. If found:
   - If exact fit: remove entry (swap with last, decrement count).
   - If larger: carve from front (`entry.base += len; entry.len -= len`), return original base.
3. If not found: return 0 (caller falls back to bump allocator).

### 4. sys_munmap changes

The existing implementation already frees physical pages. Add:

- After the physical page free loop, call `mmap_free_insert(proc, addr, len)` to return the VA to the freelist.

### 5. sys_mmap changes

Before the bump allocator:

```c
uint64_t base = mmap_free_alloc(proc, len);
if (base == 0) {
    base = proc->mmap_base;
    /* existing overflow check */
    proc->mmap_base += len;
}
```

The rest of the function (physical page allocation, zeroing, mapping) is unchanged — it operates on `base` regardless of where it came from.

**Note:** Pages from the freelist may still have stale PTEs if munmap only freed physical pages but left page table structures. vmm_map_user_page panics on double-map. The existing munmap already calls vmm_unmap_user_page which clears the PTE, so re-mapping into a freelist VA is safe.

### 6. Fork/execve/clone handling

**sys_fork:** Deep-copy `mmap_free[]` and `mmap_free_count` along with the rest of the PCB. The child gets its own address space and its own freelist.

**sys_execve:** Zero `mmap_free_count` (reset freelist) alongside the existing `mmap_base = 0x700000000000` reset.

**clone(CLONE_VM):** Threads share `aegis_process_t` fields including `mmap_free[]`. Safe on single-core (no preemption during syscalls). Forward constraint: SMP requires a spinlock.

### 7. sys_mmap: honor PROT_NONE and NX

Currently sys_mmap ignores the prot argument and always maps pages as PRESENT|USER|WRITABLE. With real mprotect, we should also fix sys_mmap to set the correct initial permissions:

- PROT_NONE: map with `VMM_FLAG_USER` only (no PRESENT — actually, don't allocate physical pages at all for PROT_NONE; just advance the VA. musl will mprotect later).
- Actually, this is tricky: PROT_NONE pages should reserve VA but not allocate physical frames. However, musl's pattern is mmap(PROT_NONE) + mprotect(PROT_READ|PROT_WRITE), so we'd need to handle the "map without physical page" case.

**Simpler approach:** Keep mapping physical pages for all mmap calls (including PROT_NONE), but set the PTE flags correctly:
- PROT_NONE → allocate+zero the page, but set PTE to `phys | VMM_FLAG_USER` (no PRESENT). The page exists in physical memory but is inaccessible.
- Wait — if PRESENT is cleared, the CPU ignores all other bits. We need to store the physical address somewhere. The PTE can hold it even without PRESENT — the CPU just won't translate through it. On munmap, we still need to find and free the physical page.

**Even simpler:** Map everything as PRESENT but use mprotect immediately after to fix permissions. This is wasteful for PROT_NONE but correct. Actually, the current behavior (map PRESENT|USER|WRITABLE for all prots) is fine as long as mprotect works — musl immediately calls mprotect after mmap to set the real permissions. The ordering is: mmap returns → mprotect runs → permissions are correct. No window for exploitation because this is all in the same syscall sequence with no preemption.

**Decision:** Keep sys_mmap mapping all pages as PRESENT|USER|WRITABLE regardless of prot argument. This matches the current behavior and is correct because musl always follows mmap with mprotect. Add NX to the default flags: `VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE | VMM_FLAG_NX`. This ensures all mmap'd pages are non-executable by default (W^X baseline).

### 8. Testing

**Test binary:** `user/mmap_test/main.c`

Test 1 — VA reuse:
```
addr1 = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0)
munmap(addr1, 4096)
addr2 = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0)
assert(addr2 == addr1)  // VA reused from freelist
```

Test 2 — mprotect PROT_NONE + SIGSEGV:
```
addr = mmap(...)
write to addr → succeeds
install SIGSEGV handler (sa_handler sets a flag and longjmp's out)
mprotect(addr, 4096, PROT_NONE)
write to addr → SIGSEGV delivered → handler runs → flag set
assert(flag set)
```

Test 3 — mprotect read-only:
```
addr = mmap(...)
*(int*)addr = 42
mprotect(addr, 4096, PROT_READ)
read addr → should succeed (value is 42)
install SIGSEGV handler
write addr → SIGSEGV
assert(flag set)
```

**Integration test:** `tests/test_mmap.py` — boots Aegis with vigil+NVMe, logs in, runs `/bin/mmap_test`, checks for `MMAP OK`.

**Existing tests:** All existing tests must continue passing. The thread test implicitly exercises the new mprotect (musl's guard page) and freelist (thread stack munmap + re-mmap).

## Files Modified

| File | Change |
|------|--------|
| `kernel/mm/vmm.c` | Add `vmm_set_user_prot()` |
| `kernel/mm/vmm.h` | Declare `vmm_set_user_prot()` |
| `kernel/syscall/sys_memory.c` | Replace mprotect stub; add freelist to munmap; add freelist-first to mmap; add NX to mmap default flags |
| `kernel/proc/proc.h` | Add `mmap_free_t`, `mmap_free[]`, `mmap_free_count` to `aegis_process_t` |
| `kernel/syscall/sys_process.c` | Reset freelist on execve; copy freelist on fork |
| `user/mmap_test/main.c` | New test binary |
| `user/mmap_test/Makefile` | New |
| `tests/test_mmap.py` | New integration test |
| `tests/run_tests.sh` | Wire test_mmap.py |
| `Makefile` | Add mmap_test to disk build |

## Forward Constraints

1. **Freelist has no lock.** Safe on single-core (no preemption during syscalls). SMP requires a spinlock on `mmap_free[]` and `mmap_base`.

2. **PROT_NONE pages still allocate physical frames.** A true demand-paging PROT_NONE (reserve VA only, fault-in on mprotect) is Phase 31+ work. Current approach wastes RAM on guard pages but is simple and correct.

3. **Intermediate page table entries keep WRITABLE+USER.** mprotect only changes leaf PTEs. The PML4e/PDPTe/PDe remain PRESENT|WRITABLE|USER. This is correct: x86-64 permission checks are the intersection of all levels, so a read-only leaf PTE is enforced even if parent entries are writable.

4. **No MAP_FIXED.** sys_mmap still rejects addr!=0. MAP_FIXED (map at a specific VA) requires evicting existing mappings, which interacts with the freelist. Deferred.

5. **File-backed mmap and MAP_SHARED deferred.** No consumers until Phase 33 (dynamic linking) and Phase 39 (IPC).
