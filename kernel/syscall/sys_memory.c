/* sys_memory.c — Memory management syscalls: brk, mmap, munmap, mprotect */
#include "sys_impl.h"

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
uint64_t
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
        /* Grow: map pages [proc->brk, arg1) into this process's PML4.
         * Zero each page before mapping — Linux brk/sbrk guarantee that
         * new heap pages are zeroed.  musl's malloc reads free-list headers
         * from fresh pages without initialising them first; stale PMM data
         * (e.g. DIR.buf_pos/buf_end != 0) causes readdir to skip the
         * getdents64 refill path and crash on garbage dirent data. */
        uint64_t va;
        for (va = proc->brk; va < arg1; va += 4096UL) {
            uint64_t phys = pmm_alloc_page();
            if (!phys)
                return proc->brk;  /* OOM — return current brk unchanged */
            vmm_zero_page(phys);
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

/* ── mmap VA freelist helpers ─────────────────────────────────────────── */

static void
mmap_free_insert(aegis_process_t *proc, uint64_t base, uint64_t len)
{
    uint32_t i;
    for (i = 0; i < proc->mmap_free_count; i++) {
        mmap_free_t *e = &proc->mmap_free[i];
        if (e->base + e->len == base) {
            e->len += len;
            uint32_t j;
            for (j = 0; j < proc->mmap_free_count; j++) {
                if (j == i) continue;
                if (e->base + e->len == proc->mmap_free[j].base) {
                    e->len += proc->mmap_free[j].len;
                    proc->mmap_free[j] = proc->mmap_free[--proc->mmap_free_count];
                    break;
                }
            }
            return;
        }
        if (base + len == e->base) {
            e->base = base;
            e->len += len;
            uint32_t j;
            for (j = 0; j < proc->mmap_free_count; j++) {
                if (j == i) continue;
                if (proc->mmap_free[j].base + proc->mmap_free[j].len == e->base) {
                    proc->mmap_free[j].len += e->len;
                    proc->mmap_free[i] = proc->mmap_free[--proc->mmap_free_count];
                    break;
                }
            }
            return;
        }
    }
    if (proc->mmap_free_count < MMAP_FREE_MAX) {
        proc->mmap_free[proc->mmap_free_count].base = base;
        proc->mmap_free[proc->mmap_free_count].len  = len;
        proc->mmap_free_count++;
    }
}

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
            if (best_len == len) break;
        }
    }
    if (best == (uint32_t)-1)
        return 0;
    uint64_t base = proc->mmap_free[best].base;
    if (proc->mmap_free[best].len == len) {
        proc->mmap_free[best] = proc->mmap_free[--proc->mmap_free_count];
    } else {
        proc->mmap_free[best].base += len;
        proc->mmap_free[best].len  -= len;
    }
    return base;
}

/*
 * sys_mmap — syscall 9
 *
 * Supports MAP_ANONYMOUS | MAP_PRIVATE only.
 * addr must be 0 (kernel chooses VA from bump allocator at 0x0000700000000000).
 * prot must be subset of PROT_READ | PROT_WRITE.
 * fd must be -1 (arg5), offset ignored (arg6).
 *
 * Each allocated page is zeroed before mapping — MAP_ANONYMOUS guarantee.
 * OOM rollback: already-mapped pages are unmapped and freed before returning -ENOMEM.
 * No capability gate — process expands its own address space only.
 */
uint64_t
sys_mmap(uint64_t arg1, uint64_t arg2, uint64_t arg3,
         uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    (void)arg6;  /* offset — not validated; musl always passes 0 for MAP_ANONYMOUS */

    aegis_process_t *proc = (aegis_process_t *)sched_current();

    /* Only support anonymous private mappings with addr==0. */
    if (arg1 != 0)
        return (uint64_t)-(int64_t)22;   /* EINVAL — fixed addr not supported */
    /* S12: Reject MAP_FIXED addresses in kernel space (defense-in-depth).
     * Currently arg1!=0 is rejected above; this guard protects against
     * future relaxation of that check. */
    if (arg1 != 0 && (arg1 >= 0xFFFF800000000000ULL || arg1 + arg2 < arg1))
        return (uint64_t)(int64_t)-22;  /* -EINVAL */
    if (!(arg4 & MAP_ANONYMOUS))
        return (uint64_t)-(int64_t)22;   /* EINVAL — file-backed not supported */
    if (arg4 & MAP_SHARED)
        return (uint64_t)-(int64_t)22;   /* EINVAL — shared not supported */
    if (arg3 & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC))
        return (uint64_t)-(int64_t)22;   /* EINVAL — unknown prot bits */
    if ((int64_t)arg5 != -1)
        return (uint64_t)-(int64_t)9;    /* EBADF */

    uint64_t len = (arg2 + 4095UL) & ~4095UL;  /* round up to page boundary */
    if (len == 0)
        return (uint64_t)-(int64_t)22;   /* EINVAL */

    /* Try freelist first; fall back to bump allocator. */
    uint64_t base = mmap_free_alloc(proc, len);
    if (base == 0) {
        base = proc->mmap_base;
        if (base + len > USER_ADDR_MAX || base + len < base)
            return (uint64_t)-(int64_t)12;  /* -ENOMEM */
    }
    uint64_t va;
    for (va = base; va < base + len; va += 4096UL) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            /* OOM: unmap already-mapped pages and return -ENOMEM.
             * MAP_FAILED = (void *)-1 — musl's allocator checks for this. */
            uint64_t v2;
            for (v2 = base; v2 < va; v2 += 4096UL) {
                uint64_t p = vmm_phys_of_user(proc->pml4_phys, v2);
                if (p) {
                    vmm_unmap_user_page(proc->pml4_phys, v2);
                    pmm_free_page(p);
                }
            }
            return (uint64_t)-(int64_t)12;  /* -ENOMEM */
        }
        /* MAP_ANONYMOUS guarantee: zero the page before mapping it.
         * musl's heap allocator reads free-list metadata from fresh pages;
         * stale PMM data would corrupt the allocator. */
        vmm_zero_page(phys);
        vmm_map_user_page(proc->pml4_phys, va, phys,
                          VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE | VMM_FLAG_NX);
    }

    /* Advance bump allocator only if we used it (not freelist). */
    if (base >= proc->mmap_base)
        proc->mmap_base = base + len;
    return base;
}

/*
 * sys_munmap — syscall 11
 *
 * arg1 = addr (must be page-aligned)
 * arg2 = length
 *
 * Frees physical pages for [addr, addr+len). Does not reclaim VA
 * (bump allocator — VA is not reused in Phase 14).
 * Returns 0 on success, -EINVAL if addr is not page-aligned.
 */
uint64_t
sys_munmap(uint64_t arg1, uint64_t arg2)
{
    if (arg1 & 0xFFFUL)
        return (uint64_t)-(int64_t)22;   /* EINVAL — not page-aligned */

    aegis_process_t *proc = (aegis_process_t *)sched_current();
    uint64_t len = (arg2 + 4095UL) & ~4095UL;
    uint64_t va;
    for (va = arg1; va < arg1 + len; va += 4096UL) {
        uint64_t phys = vmm_phys_of_user(proc->pml4_phys, va);
        if (phys) {
            vmm_unmap_user_page(proc->pml4_phys, va);
            pmm_free_page(phys);
        }
    }

    /* Return VA range to freelist for reuse by future mmap calls. */
    mmap_free_insert(proc, arg1, len);

    return 0;
}

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
