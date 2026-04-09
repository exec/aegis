/* sys_memory.c — Memory management syscalls: brk, mmap, munmap, mprotect */
#include "sys_impl.h"
#include "sched.h"
#include "proc.h"
#include "vmm.h"
#include "pmm.h"
#include "vfs.h"
#include "vma.h"
#include "memfd.h"

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

    /* M2: reject shrink below the initial ELF brk — prevents freeing
     * ELF segment pages via brk manipulation. */
    if (arg1 < proc->brk_base)
        return proc->brk;

    /* Page-align upward so proc->brk is always page-aligned */
    arg1 = (arg1 + 4095UL) & ~4095UL;

    uint64_t old_brk = proc->brk;

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
                vma_insert(proc, old_brk, proc->brk - old_brk,
                           0x01 | 0x02, VMA_HEAP);
        }
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

        /* Update heap VMA */
        if (proc->vma_table) {
            uint32_t vi;
            for (vi = 0; vi < proc->vma_count; vi++) {
                if (proc->vma_table[vi].type == VMA_HEAP) {
                    if (proc->brk <= proc->vma_table[vi].base) {
                        vma_remove(proc, proc->vma_table[vi].base,
                                   proc->vma_table[vi].len);
                    } else {
                        proc->vma_table[vi].len = proc->brk -
                                                  proc->vma_table[vi].base;
                    }
                    break;
                }
            }
        }
    }

    return proc->brk;
}

/* ── mmap VA freelist helpers ─────────────────────────────────────────── */

/*
 * M2 audit fix: mmap_free_insert / mmap_free_alloc take proc->mmap_free_lock
 * for their full bodies. Two threads sharing a VM (CLONE_VM) can race on the
 * freelist — latent on single-core, corrupting on SMP. The lock is per-process
 * and does not nest with any other kernel lock, so no ordering constraints.
 * Callers (sys_mmap, sys_munmap) do not hold other locks at the call site.
 */
static void
mmap_free_insert(aegis_process_t *proc, uint64_t base, uint64_t len)
{
    irqflags_t fl = spin_lock_irqsave(&proc->mmap_free_lock);
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
            spin_unlock_irqrestore(&proc->mmap_free_lock, fl);
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
            spin_unlock_irqrestore(&proc->mmap_free_lock, fl);
            return;
        }
    }
    if (proc->mmap_free_count < MMAP_FREE_MAX) {
        proc->mmap_free[proc->mmap_free_count].base = base;
        proc->mmap_free[proc->mmap_free_count].len  = len;
        proc->mmap_free_count++;
    }
    spin_unlock_irqrestore(&proc->mmap_free_lock, fl);
}

static uint64_t
mmap_free_alloc(aegis_process_t *proc, uint64_t len)
{
    irqflags_t fl = spin_lock_irqsave(&proc->mmap_free_lock);
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
    if (best == (uint32_t)-1) {
        spin_unlock_irqrestore(&proc->mmap_free_lock, fl);
        return 0;
    }
    uint64_t base = proc->mmap_free[best].base;
    if (proc->mmap_free[best].len == len) {
        proc->mmap_free[best] = proc->mmap_free[--proc->mmap_free_count];
    } else {
        proc->mmap_free[best].base += len;
        proc->mmap_free[best].len  -= len;
    }
    spin_unlock_irqrestore(&proc->mmap_free_lock, fl);
    return base;
}

/*
 * sys_mmap — syscall 9
 *
 * Supports MAP_ANONYMOUS | MAP_PRIVATE, MAP_FIXED, and file-backed private mappings.
 *
 * MAP_FIXED (addr != 0, MAP_FIXED set): map at exact address; silently unmap
 * existing pages first.  addr must be page-aligned, below 0x800000000000.
 *
 * File-backed (fd != -1, MAP_ANONYMOUS not set): read file content into mapped
 * pages via VFS.  MAP_PRIVATE semantics — pages are independent of file after
 * mapping.  offset must be page-aligned.
 *
 * Each allocated page is zeroed before mapping — MAP_ANONYMOUS guarantee.
 * OOM rollback: already-mapped pages are unmapped and freed before returning -ENOMEM.
 * No capability gate — process expands its own address space only.
 */
uint64_t
sys_mmap(uint64_t arg1, uint64_t arg2, uint64_t arg3,
         uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    uint64_t addr  = arg1;
    uint64_t prot  = arg3;
    uint64_t flags = arg4;
    int64_t  fd    = (int64_t)arg5;
    uint64_t off   = arg6;

    uint64_t len = (arg2 + 4095UL) & ~4095UL;
    if (len == 0)
        return (uint64_t)-(int64_t)22;   /* EINVAL */
    if (prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC))
        return (uint64_t)-(int64_t)22;   /* EINVAL */
    int is_shared = (flags & MAP_SHARED) != 0;

    int file_backed = !(flags & MAP_ANONYMOUS) && fd != -1;
    if (!file_backed && !(flags & MAP_ANONYMOUS))
        return (uint64_t)-(int64_t)22;   /* EINVAL */
    if (file_backed && (off & 0xFFFUL))
        return (uint64_t)-(int64_t)22;   /* EINVAL — offset not page-aligned */

    int is_fixed = (flags & MAP_FIXED) && addr != 0;
    uint64_t base;

    if (is_fixed) {
        if (addr & 0xFFFUL)
            return (uint64_t)-(int64_t)22;   /* EINVAL — not page-aligned */
        if (addr >= 0x800000000000ULL || addr + len > 0x800000000000ULL ||
            addr + len < addr)
            return (uint64_t)-(int64_t)22;   /* EINVAL — out of user range */
        /* Silently unmap existing pages in the target range */
        uint64_t va;
        for (va = addr; va < addr + len; va += 4096UL) {
            uint64_t phys = vmm_phys_of_user(proc->pml4_phys, va);
            if (phys) {
                vmm_unmap_user_page(proc->pml4_phys, va);
                pmm_free_page(phys);
            }
        }
        vma_remove(proc, addr, len);
        base = addr;
    } else {
        if (addr != 0)
            return (uint64_t)-(int64_t)22;   /* EINVAL — addr must be 0 */
        /* Try freelist first; fall back to bump allocator. */
        base = mmap_free_alloc(proc, len);
        if (base == 0) {
            base = proc->mmap_base;
            if (base + len > USER_ADDR_MAX || base + len < base)
                return (uint64_t)-(int64_t)12;  /* -ENOMEM */
        } else {
            /* H3: freelist-returned base needs the same overflow check. */
            if (base + len > USER_ADDR_MAX || base + len < base)
                return (uint64_t)-(int64_t)12;  /* -ENOMEM */
        }
    }

    /* PTE flags: NX by default; clear NX only for PROT_EXEC */
    uint64_t map_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_NX;
    if (prot & PROT_WRITE)
        map_flags |= VMM_FLAG_WRITABLE;
    if (prot & PROT_EXEC)
        map_flags &= ~VMM_FLAG_NX;

    /* MAP_SHARED path: map memfd's physical pages directly */
    if (is_shared && file_backed) {
        extern const vfs_ops_t g_memfd_ops;
        if ((uint32_t)fd >= PROC_MAX_FDS ||
            !proc->fd_table->fds[(uint32_t)fd].ops ||
            proc->fd_table->fds[(uint32_t)fd].ops != &g_memfd_ops)
            return (uint64_t)-(int64_t)22;  /* EINVAL: MAP_SHARED only for memfd */

        uint32_t mid = (uint32_t)(uintptr_t)proc->fd_table->fds[(uint32_t)fd].priv;
        extern memfd_t *memfd_get(uint32_t id);
        memfd_t *mf = memfd_get(mid);
        if (!mf) return (uint64_t)-(int64_t)22;
        if (len > mf->size)
            return (uint64_t)-(int64_t)22;  /* mapping larger than memfd */

        uint32_t num_pages = (uint32_t)(len / 4096);
        uint64_t va;
        for (va = base; va < base + len; va += 4096UL) {
            uint32_t pi = (uint32_t)((va - base) / 4096);
            if (pi >= mf->page_count || !mf->phys_pages[pi])
                return (uint64_t)-(int64_t)22;
            vmm_map_user_page(proc->pml4_phys, va, mf->phys_pages[pi], map_flags);
        }
        (void)num_pages;

        if (!is_fixed && base >= proc->mmap_base)
            proc->mmap_base = base + len;

        vma_insert(proc, base, len, (uint32_t)(prot & 0x07), VMA_SHARED);
        return base;
    }

    /* Allocate and map pages (private anonymous or private file-backed) */
    uint64_t va;
    for (va = base; va < base + len; va += 4096UL) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            /* OOM: unmap already-mapped pages and return -ENOMEM */
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
        vmm_zero_page(phys);
        vmm_map_user_page(proc->pml4_phys, va, phys, map_flags);
    }

    /* File-backed: copy file content into the mapped pages */
    if (file_backed) {
        if ((uint32_t)fd < PROC_MAX_FDS &&
            proc->fd_table->fds[(uint32_t)fd].ops &&
            proc->fd_table->fds[(uint32_t)fd].ops->read) {
            vfs_file_t *f = &proc->fd_table->fds[(uint32_t)fd];
            uint64_t file_bytes = len;
            if (f->size > 0 && off < f->size) {
                uint64_t avail = f->size - off;
                if (file_bytes > avail)
                    file_bytes = avail;
            } else if (f->size > 0 && off >= f->size) {
                file_bytes = 0;
            }
            if (file_bytes > 0) {
                uint8_t chunk[4096];
                uint64_t copied = 0;
                while (copied < file_bytes) {
                    uint64_t want = file_bytes - copied;
                    if (want > 4096) want = 4096;
                    int rr = f->ops->read(f->priv, chunk,
                                          off + copied, want);
                    if (rr <= 0) break;
                    vmm_write_user_bytes(proc->pml4_phys,
                                         base + copied,
                                         chunk, (uint64_t)rr);
                    copied += (uint64_t)rr;
                }
            }
        }
    }

    /* Advance bump allocator only if we used it (not freelist or MAP_FIXED). */
    if (!is_fixed && base >= proc->mmap_base)
        proc->mmap_base = base + len;

    vma_insert(proc, base, len, (uint32_t)(prot & 0x07), VMA_MMAP);

    return base;
}

/*
 * sys_munmap — syscall 11
 *
 * arg1 = addr (must be page-aligned)
 * arg2 = length
 *
 * Frees physical pages for [addr, addr+len) and returns VA to the
 * per-process freelist for reuse by future mmap calls.
 * Returns 0 on success, -EINVAL if addr is not page-aligned.
 */
uint64_t
sys_munmap(uint64_t arg1, uint64_t arg2)
{
    if (arg1 & 0xFFFUL)
        return (uint64_t)-(int64_t)22;   /* EINVAL — not page-aligned */

    uint64_t len = (arg2 + 4095UL) & ~4095UL;

    /* C7: reject kernel addresses and overflow — prevent kernel VAs from
     * being inserted into the mmap freelist. */
    if (arg1 >= 0x00007FFFFFFFFFFFULL ||
        arg1 + len > 0x00007FFFFFFFFFFFULL ||
        arg1 + len < arg1)
        return (uint64_t)-(int64_t)22;   /* EINVAL */

    aegis_process_t *proc = (aegis_process_t *)sched_current();

    /* Check if this is a shared mapping — don't free physical pages
     * (they belong to the memfd, not this process). */
    int is_shared_vma = 0;
    if (proc->vma_table) {
        for (uint32_t i = 0; i < proc->vma_count; i++) {
            vma_entry_t *v = &proc->vma_table[i];
            if (v->base == arg1 && v->type == VMA_SHARED) {
                is_shared_vma = 1;
                break;
            }
        }
    }

    uint64_t va;
    for (va = arg1; va < arg1 + len; va += 4096UL) {
        /* H4: use vmm_phys_of_user_raw to find physical frames even when
         * PRESENT is cleared (PROT_NONE pages from mprotect). Without this,
         * munmap leaks physical frames for PROT_NONE pages. */
        uint64_t phys = vmm_phys_of_user_raw(proc->pml4_phys, va);
        if (phys) {
            vmm_unmap_user_page(proc->pml4_phys, va);
            if (!is_shared_vma)
                pmm_free_page(phys);
        }
    }

    /* Return VA range to freelist for reuse by future mmap calls. */
    mmap_free_insert(proc, arg1, len);

    vma_remove(proc, arg1, len);

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

    vma_update_prot(proc, addr, rlen, (uint32_t)(prot & 0x07));

    return 0;
}

/* ── sys_memfd_create ─────────────────────────────────────────────────── */

uint64_t sys_memfd_create(uint64_t name_ptr, uint64_t flags)
{
    (void)flags;
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE, CAP_KIND_IPC, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)130;  /* ENOCAP */

    char name[32] = {0};
    if (name_ptr && user_ptr_valid(name_ptr, 1)) {
        for (int i = 0; i < 31; i++) {
            uint8_t c;
            copy_from_user(&c, (const void *)(uintptr_t)(name_ptr + (uint64_t)i), 1);
            if (!c) break;
            name[i] = (char)c;
        }
    }

    int mid = memfd_alloc(name);
    if (mid < 0) return (uint64_t)-(int64_t)24;  /* EMFILE */

    int fd = memfd_open_fd((uint32_t)mid, proc);
    if (fd < 0) {
        memfd_t *mf = memfd_get((uint32_t)mid);
        if (mf) { mf->refcount = 0; mf->in_use = 0; }
        return (uint64_t)-(int64_t)24;
    }
    return (uint64_t)fd;
}

/* ── sys_ftruncate ────────────────────────────────────────────────────── */

uint64_t sys_ftruncate(uint64_t fd_arg, uint64_t length)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    memfd_t *mf = memfd_from_fd((int)fd_arg, proc);
    if (!mf) return (uint64_t)-(int64_t)22;  /* EINVAL: not a memfd */

    uint32_t mid = (uint32_t)(uintptr_t)proc->fd_table->fds[(int)fd_arg].priv;
    int rc = memfd_truncate(mid, length);
    return rc < 0 ? (uint64_t)(int64_t)rc : 0;
}
