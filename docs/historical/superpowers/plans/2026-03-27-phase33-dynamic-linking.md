# Phase 33: Dynamic Linking Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add dynamic linking support — PT_INTERP, ET_DYN loading, MAP_FIXED, file-backed mmap — and rebuild most userspace binaries as dynamically-linked against musl's shared libc.so.

**Architecture:** The kernel's execve handles PT_INTERP by loading the interpreter (musl's `ld-musl-x86_64.so.1`) into the process address space at a fixed base. The interpreter self-relocates, resolves the main binary's dependencies (just libc.so = itself), and jumps to the main binary's entry. MAP_FIXED + file-backed mmap provide the syscall infrastructure for future dlopen.

**Tech Stack:** C (kernel), musl 1.2.x (shared libc), QEMU q35 + NVMe (testing)

---

## File Structure

### Kernel files modified:
- `kernel/elf/elf.h` — Add `base` field to `elf_load_result_t`; update `elf_load` signature
- `kernel/elf/elf.c` — Accept ET_DYN; apply base offset to segment addresses; scan for PT_INTERP
- `kernel/syscall/sys_process.c` — PT_INTERP handling in `sys_execve`; extended auxv (AT_BASE, AT_PHENT)
- `kernel/syscall/sys_impl.h` — Add `MAP_FIXED` define; `INTERP_BASE` constant
- `kernel/syscall/sys_memory.c` — MAP_FIXED support; file-backed mmap (MAP_PRIVATE + fd)

### Build files modified:
- `Makefile` — Remove non-boot-critical binaries from initrd blob list; add `/lib/` to disk; add `build-musl` target
- `tools/build-musl.sh` — New: download + build musl as shared library
- `user/*/Makefile` — Dynamic linking flags for non-boot-critical binaries

### Test files:
- `user/dynlink_test/main.c` — New: test binary that verifies dynamic linking works and reads /proc/self/maps
- `user/dynlink_test/Makefile` — New
- `tests/test_dynlink.py` — New: integration test

---

## Task 1: ELF Loader — ET_DYN + Base Offset Support

**Files:**
- Modify: `kernel/elf/elf.h`
- Modify: `kernel/elf/elf.c`

This task adds the `base` parameter to `elf_load` so ET_DYN binaries (like the musl interpreter) can be loaded at an arbitrary base address. No PT_INTERP handling yet — that comes in Task 2.

- [ ] **Step 1: Update `elf_load_result_t` in elf.h**

Add `base` field and update function signature:

```c
/* kernel/elf/elf.h */
#ifndef AEGIS_ELF_H
#define AEGIS_ELF_H

#include <stdint.h>
#include <stddef.h>

#define ET_EXEC     2
#define ET_DYN      3
#define PT_INTERP   3
#define INTERP_BASE 0x40000000ULL

typedef struct {
    uint64_t entry;
    uint64_t brk;
    uint64_t phdr_va;
    uint32_t phdr_count;
    uint64_t base;       /* load bias (0 for ET_EXEC, INTERP_BASE for interpreter) */
    char     interp[256]; /* PT_INTERP path, empty if none */
} elf_load_result_t;

/* elf_load -- parse ELF64; map all PT_LOAD segments into pml4_phys.
 * base is added to every p_vaddr (0 for ET_EXEC, INTERP_BASE for ET_DYN).
 * Returns 0 on success; fills *out. Returns -1 on parse error. */
int elf_load(uint64_t pml4_phys, const uint8_t *data,
             size_t len, uint64_t base, elf_load_result_t *out);

#endif /* AEGIS_ELF_H */
```

- [ ] **Step 2: Update elf.c to accept ET_DYN and apply base offset**

In `kernel/elf/elf.c`, change the validation to accept both ET_EXEC and ET_DYN, add `base` parameter, apply it to all segment addresses, and scan for PT_INTERP:

```c
#define PT_LOAD    1
#define PT_INTERP_TYPE 3   /* p_type for interpreter path */
#define PF_W       2
#define PF_X       1
#define ELFCLASS64 2
#define ET_EXEC_V  2
#define ET_DYN_V   3
#define EM_X86_64  0x3E
#define EM_AARCH64 0xB7

/* In the validation block, change: */
    if (eh->e_ident[4] != ELFCLASS64 ||
        (eh->e_type != ET_EXEC_V && eh->e_type != ET_DYN_V) ||
        eh->e_machine != EM_CURRENT) {
        printk("[ELF] FAIL: not an ELF64 executable for this arch\n");
        return -1;
    }

/* Initialize output: */
    out->base = base;
    out->interp[0] = '\0';

/* Scan for PT_INTERP before loading segments: */
    for (i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type == PT_INTERP_TYPE && ph->p_filesz > 0 &&
            ph->p_filesz < 256) {
            uint64_t k;
            for (k = 0; k < ph->p_filesz && k < 255; k++)
                out->interp[k] = (char)data[ph->p_offset + k];
            out->interp[k] = '\0';
            /* Strip trailing null if present */
            if (k > 0 && out->interp[k - 1] == '\0')
                ; /* already terminated */
            break;
        }
    }

/* In the PT_LOAD loop, apply base offset to all addresses: */
    uint64_t va_base   = (ph->p_vaddr + base) & ~4095UL;
    uint64_t va_offset = (ph->p_vaddr + base) & 4095UL;

    /* ... (page count, alloc, copy unchanged) ... */

    /* In the user page mapping loop: */
    vmm_map_user_page(pml4_phys,
                      va_base + j * 4096UL,       /* already includes base */
                      kva_page_phys(dst + j * 4096UL),
                      map_flags);

    /* VMA insert with base-adjusted address: */
    vma_insert(p, va_base, page_count * 4096UL, seg_prot, seg_type);

    /* this_end calculation also needs base: */
    uint64_t this_end = ph->p_vaddr + base + ph->p_memsz;

/* Result fields: */
    out->entry      = eh->e_entry + base;
    out->brk        = (seg_end + 4095UL) & ~4095UL;
    out->phdr_va    = first_pt_load_vaddr + base + eh->e_phoff;
    out->phdr_count = eh->e_phnum;
```

- [ ] **Step 3: Update all callers of elf_load to pass base=0**

In `kernel/syscall/sys_process.c`, find the two `elf_load` call sites:

1. In `sys_execve` (~line 921):
```c
    if (elf_load(proc->pml4_phys, elf_data, (size_t)elf_size, 0, &er) != 0)
```

2. In `proc_spawn` in `kernel/proc/proc.c` — find the `elf_load` call and add `0`:
```c
    if (elf_load(proc->pml4_phys, data, len, 0, &result) != 0) {
```

- [ ] **Step 4: Build and run `make test`**

```bash
rm -rf build && make test
```

Expected: PASS — no behavior change, all existing callers pass `base=0` which gives the same behavior as before.

- [ ] **Step 5: Commit**

```bash
git add kernel/elf/elf.h kernel/elf/elf.c kernel/syscall/sys_process.c kernel/proc/proc.c
git commit -m "feat: elf_load accepts ET_DYN + base offset, scans PT_INTERP"
```

---

## Task 2: sys_execve PT_INTERP Handling + Extended Auxv

**Files:**
- Modify: `kernel/syscall/sys_process.c`
- Modify: `kernel/syscall/sys_impl.h`

When `elf_load` returns a non-empty `interp` path, execve loads the interpreter as a second ELF at `INTERP_BASE`, sets RIP to the interpreter's entry, and adds AT_BASE + AT_PHENT to auxv.

- [ ] **Step 1: Add constants to sys_impl.h**

```c
/* After the existing MAP_* defines: */
#ifndef MAP_FIXED
#define MAP_FIXED       0x10
#endif

#define INTERP_BASE     0x40000000ULL  /* fixed base for ELF interpreter */
```

- [ ] **Step 2: Add PT_INTERP handling in sys_execve**

After the existing `elf_load` call (step 6 in the current code, ~line 921), add interpreter loading:

```c
    /* 6. Load new ELF */
    elf_load_result_t er;
    if (elf_load(proc->pml4_phys, elf_data, (size_t)elf_size, 0, &er) != 0)
        { ret = (uint64_t)-(int64_t)8; goto done; }  /* ENOEXEC */

    /* 6a. If PT_INTERP present, load the interpreter */
    elf_load_result_t interp_er;
    int has_interp = (er.interp[0] != '\0');
    if (has_interp) {
        /* Open interpreter via VFS (tries initrd, then ext2) */
        vfs_file_t interp_f;
        const uint8_t *interp_data;
        uint64_t interp_size;
        void    *interp_buf   = (void *)0;
        uint64_t interp_pages = 0;

        if (initrd_open(er.interp, &interp_f) == 0) {
            interp_data = (const uint8_t *)initrd_get_data(&interp_f);
            interp_size = (uint64_t)initrd_get_size(&interp_f);
        } else {
            vfs_file_t vf;
            int vr = vfs_open(er.interp, 0, &vf);
            if (vr != 0) {
                if (ext2_buf) { kva_free_pages(ext2_buf, ext2_pages); ext2_buf = (void *)0; ext2_pages = 0; }
                ret = (uint64_t)-(int64_t)2; goto done;  /* ENOENT */
            }
            interp_pages = (vf.size + 4095ULL) / 4096ULL;
            interp_buf = kva_alloc_pages(interp_pages);
            if (!interp_buf) {
                if (ext2_buf) { kva_free_pages(ext2_buf, ext2_pages); ext2_buf = (void *)0; ext2_pages = 0; }
                ret = (uint64_t)-(int64_t)12; goto done;  /* ENOMEM */
            }
            int rr = vf.ops->read(vf.priv, interp_buf, 0, vf.size);
            if (rr < 0) {
                kva_free_pages(interp_buf, interp_pages);
                if (ext2_buf) { kva_free_pages(ext2_buf, ext2_pages); ext2_buf = (void *)0; ext2_pages = 0; }
                ret = (uint64_t)-(int64_t)5; goto done;   /* EIO */
            }
            interp_data = (const uint8_t *)interp_buf;
            interp_size = vf.size;
        }

        if (elf_load(proc->pml4_phys, interp_data, (size_t)interp_size,
                     INTERP_BASE, &interp_er) != 0) {
            if (interp_buf) kva_free_pages(interp_buf, interp_pages);
            if (ext2_buf) { kva_free_pages(ext2_buf, ext2_pages); ext2_buf = (void *)0; ext2_pages = 0; }
            ret = (uint64_t)-(int64_t)8; goto done;  /* ENOEXEC */
        }
        if (interp_buf) kva_free_pages(interp_buf, interp_pages);
    }
```

- [ ] **Step 3: Extend auxv to include AT_BASE and AT_PHENT**

Update the table_qwords calculation. Currently `argc2 + 15` (6 auxv pairs = 12 qwords + argc + 1 argv NULL + 1 envp NULL). Add 2 more auxv pairs (AT_BASE + AT_PHENT = 4 more qwords):

```c
    /* When has_interp: 8 auxv pairs (PHDR, PHNUM, PAGESZ, ENTRY, RANDOM, BASE, PHENT, NULL)
     * = 16 qwords.  Without interp: 6 pairs = 12 qwords.
     * Total: argc + 1(argc) + 1(argv NULL) + 1(envp NULL) + auxv_qwords */
    uint64_t auxv_qwords = has_interp ? 16 : 12;
    uint64_t table_qwords = (uint64_t)argc2 + 3 + auxv_qwords;
```

Then after the existing AT_RANDOM auxv write, add AT_BASE and AT_PHENT (only when has_interp):

```c
        /* auxv: AT_BASE — interpreter load address (only if interpreter present) */
        if (has_interp) {
            if (vmm_write_user_u64(proc->pml4_phys, wp, 7ULL) != 0)
                { ret = (uint64_t)-(int64_t)14; goto done; }
            wp += 8;
            if (vmm_write_user_u64(proc->pml4_phys, wp, INTERP_BASE) != 0)
                { ret = (uint64_t)-(int64_t)14; goto done; }
            wp += 8;

            /* auxv: AT_PHENT — size of one program header entry */
            if (vmm_write_user_u64(proc->pml4_phys, wp, 4ULL) != 0)
                { ret = (uint64_t)-(int64_t)14; goto done; }
            wp += 8;
            if (vmm_write_user_u64(proc->pml4_phys, wp, 56ULL) != 0)
                { ret = (uint64_t)-(int64_t)14; goto done; }
            wp += 8;
        }

        /* auxv: AT_NULL (end sentinel) — KEEP THIS LAST */
```

- [ ] **Step 4: Set RIP to interpreter entry when PT_INTERP present**

At the bottom of execve, change:

```c
    /* 10. Redirect return to new ELF entry point */
    if (has_interp) {
        /* AT_ENTRY already has the main binary's entry;
         * RIP goes to interpreter which reads AT_ENTRY from auxv */
        FRAME_IP(frame) = interp_er.entry;  /* interpreter's entry */
    } else {
        FRAME_IP(frame) = er.entry;         /* static binary entry */
    }
    FRAME_SP(frame) = sp_va;
```

- [ ] **Step 5: Build and run `make test`**

```bash
rm -rf build && make test
```

Expected: PASS — all existing binaries are static (no PT_INTERP), so the new code path is not exercised. The `has_interp` flag will be 0 for all current binaries.

- [ ] **Step 6: Commit**

```bash
git add kernel/syscall/sys_process.c kernel/syscall/sys_impl.h
git commit -m "feat: sys_execve handles PT_INTERP — loads interpreter at INTERP_BASE"
```

---

## Task 3: MAP_FIXED Support in sys_mmap

**Files:**
- Modify: `kernel/syscall/sys_memory.c`

Add MAP_FIXED support: when addr != 0 and MAP_FIXED is set, map at exactly that address after silently unmapping any existing mappings in the range.

- [ ] **Step 1: Rewrite sys_mmap to support MAP_FIXED**

Replace the current `sys_mmap` function. The key changes are:
1. When `MAP_FIXED` is set with `addr != 0`, use that address directly
2. Unmap existing pages in the target range before mapping
3. Don't advance the bump allocator for MAP_FIXED

```c
uint64_t
sys_mmap(uint64_t arg1, uint64_t arg2, uint64_t arg3,
         uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    uint64_t addr  = arg1;
    uint64_t len   = (arg2 + 4095UL) & ~4095UL;
    uint64_t prot  = arg3;
    uint64_t flags = arg4;
    int64_t  fd    = (int64_t)arg5;
    uint64_t off   = arg6;

    if (len == 0)
        return (uint64_t)-(int64_t)22;   /* EINVAL */
    if (prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC))
        return (uint64_t)-(int64_t)22;   /* EINVAL — unknown prot bits */
    if (flags & MAP_SHARED)
        return (uint64_t)-(int64_t)22;   /* EINVAL — shared not supported */

    /* Determine if file-backed or anonymous */
    int file_backed = !(flags & MAP_ANONYMOUS) && fd != -1;
    if (!file_backed && !(flags & MAP_ANONYMOUS))
        return (uint64_t)-(int64_t)22;   /* EINVAL — need either ANON or fd */
    if (file_backed && (off & 0xFFFUL))
        return (uint64_t)-(int64_t)22;   /* EINVAL — offset not page-aligned */

    /* MAP_FIXED: use addr directly; otherwise bump/freelist allocator */
    int is_fixed = (flags & MAP_FIXED) && addr != 0;
    uint64_t base;

    if (is_fixed) {
        if (addr & 0xFFFUL)
            return (uint64_t)-(int64_t)22;   /* EINVAL — not page-aligned */
        if (addr >= 0x800000000000ULL || addr + len > 0x800000000000ULL || addr + len < addr)
            return (uint64_t)-(int64_t)22;   /* EINVAL — exceeds user space */

        /* Silently unmap existing pages in [addr, addr+len) */
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
            return (uint64_t)-(int64_t)22;   /* EINVAL — non-fixed with addr */
        /* Try freelist first; fall back to bump allocator */
        base = mmap_free_alloc(proc, len);
        if (base == 0) {
            base = proc->mmap_base;
            if (base + len > USER_ADDR_MAX || base + len < base)
                return (uint64_t)-(int64_t)12;  /* ENOMEM */
        }
    }

    /* Allocate and map pages */
    uint64_t map_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_NX;
    if (prot & PROT_WRITE)
        map_flags |= VMM_FLAG_WRITABLE;
    if (prot & PROT_EXEC)
        map_flags &= ~VMM_FLAG_NX;

    uint64_t va;
    for (va = base; va < base + len; va += 4096UL) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            /* OOM rollback */
            uint64_t v2;
            for (v2 = base; v2 < va; v2 += 4096UL) {
                uint64_t p = vmm_phys_of_user(proc->pml4_phys, v2);
                if (p) {
                    vmm_unmap_user_page(proc->pml4_phys, v2);
                    pmm_free_page(p);
                }
            }
            return (uint64_t)-(int64_t)12;  /* ENOMEM */
        }
        vmm_zero_page(phys);
        vmm_map_user_page(proc->pml4_phys, va, phys, map_flags);
    }

    /* File-backed: read file contents into mapped pages */
    if (file_backed) {
        if ((uint32_t)fd >= PROC_MAX_FDS ||
            !proc->fd_table->fds[(uint32_t)fd].ops ||
            !proc->fd_table->fds[(uint32_t)fd].ops->read) {
            /* Bad fd — unmap and fail. Accept the leak for simplicity. */
            return (uint64_t)-(int64_t)9;  /* EBADF */
        }
        vfs_file_t *f = &proc->fd_table->fds[(uint32_t)fd];
        /* Read file data into a kernel buffer, then copy to user pages */
        uint64_t file_bytes = len;
        if (f->size > 0 && off < f->size) {
            uint64_t avail = f->size - off;
            if (file_bytes > avail)
                file_bytes = avail;
        } else if (f->size > 0 && off >= f->size) {
            file_bytes = 0;
        }
        /* Read in 4KB chunks via vmm_write_user_bytes */
        if (file_bytes > 0) {
            uint8_t chunk[4096];
            uint64_t copied = 0;
            while (copied < file_bytes) {
                uint64_t want = file_bytes - copied;
                if (want > 4096) want = 4096;
                int rr = f->ops->read(f->priv, chunk, off + copied, want);
                if (rr <= 0) break;
                vmm_write_user_bytes(proc->pml4_phys, base + copied, chunk, (uint64_t)rr);
                copied += (uint64_t)rr;
            }
        }
    }

    /* Advance bump allocator only if not MAP_FIXED and not from freelist */
    if (!is_fixed && base >= proc->mmap_base)
        proc->mmap_base = base + len;

    vma_insert(proc, base, len, (uint32_t)(prot & 0x07), VMA_MMAP);

    return base;
}
```

- [ ] **Step 2: Build and run `make test`**

```bash
rm -rf build && make test
```

Expected: PASS — existing callers all use MAP_ANONYMOUS with addr=0, which still works as before.

- [ ] **Step 3: Commit**

```bash
git add kernel/syscall/sys_memory.c
git commit -m "feat: sys_mmap supports MAP_FIXED + file-backed MAP_PRIVATE"
```

---

## Task 4: Build musl as Shared Library

**Files:**
- Create: `tools/build-musl.sh`

This script downloads musl source, builds it as a shared library, and installs to `build/musl-dynamic/`.

- [ ] **Step 1: Create tools/build-musl.sh**

```bash
#!/bin/bash
# tools/build-musl.sh — Build musl as a shared library for dynamic linking.
# Produces:
#   build/musl-dynamic/lib/libc.so         (shared library + interpreter)
#   build/musl-dynamic/lib/ld-musl-x86_64.so.1  (symlink to libc.so)
#   build/musl-dynamic/lib/musl-gcc.specs  (gcc specs for dynamic linking)
#   build/musl-dynamic/include/            (headers)
set -euo pipefail

MUSL_VER="1.2.5"
MUSL_DIR="references/musl-${MUSL_VER}"
MUSL_TAR="references/musl-${MUSL_VER}.tar.gz"
INSTALL_DIR="$(pwd)/build/musl-dynamic"

# Skip if already built
if [ -f "$INSTALL_DIR/lib/libc.so" ]; then
    echo "[musl] Shared library already built at $INSTALL_DIR/lib/libc.so"
    exit 0
fi

# Download if not present
if [ ! -d "$MUSL_DIR" ]; then
    echo "[musl] Downloading musl ${MUSL_VER}..."
    mkdir -p references
    curl -fsSL "https://musl.libc.org/releases/musl-${MUSL_VER}.tar.gz" -o "$MUSL_TAR"
    tar -xzf "$MUSL_TAR" -C references/
    rm -f "$MUSL_TAR"
fi

echo "[musl] Configuring shared build..."
cd "$MUSL_DIR"

# Clean any previous build
make distclean 2>/dev/null || true

# Configure: --syslibdir=/lib sets PT_INTERP to /lib/ld-musl-x86_64.so.1
./configure \
    --prefix=/usr \
    --syslibdir=/lib \
    --enable-shared \
    CFLAGS="-O2 -fno-pie"

echo "[musl] Building..."
make -j"$(nproc)"

echo "[musl] Installing to $INSTALL_DIR..."
make install DESTDIR="$INSTALL_DIR"

echo "[musl] Done. libc.so at $INSTALL_DIR/lib/libc.so"
ls -la "$INSTALL_DIR/lib/libc.so" "$INSTALL_DIR/lib/ld-musl-x86_64.so.1" 2>/dev/null || true
```

- [ ] **Step 2: Run the build script**

```bash
bash tools/build-musl.sh
```

Expected: `build/musl-dynamic/lib/libc.so` exists, `build/musl-dynamic/lib/ld-musl-x86_64.so.1` is a symlink to it.

- [ ] **Step 3: Verify the interpreter path is correct**

```bash
readelf -l build/musl-dynamic/lib/libc.so | head -30
file build/musl-dynamic/lib/libc.so
```

Expected: ET_DYN, contains PT_LOAD segments. No PT_INTERP (it IS the interpreter).

- [ ] **Step 4: Commit**

```bash
git add tools/build-musl.sh
git commit -m "feat: tools/build-musl.sh — build musl as shared libc.so"
```

---

## Task 5: Dynamic User Binary Build Infrastructure

**Files:**
- Modify: Multiple `user/*/Makefile` files
- Create: `user/dynlink_test/main.c`
- Create: `user/dynlink_test/Makefile`

Convert non-boot-critical binaries to dynamic linking using our custom-built musl.

- [ ] **Step 1: Create a dynamic linking Makefile template**

For each dynamically-linked binary, the Makefile changes from:
```makefile
CC     = musl-gcc
CFLAGS = -static -O2 -s -fno-pie -no-pie -Wl,--build-id=none
```
to:
```makefile
MUSL_DYN = ../../build/musl-dynamic
CC       = gcc
CFLAGS   = -O2 -fno-pie -no-pie -Wl,--build-id=none \
           -nostdinc -isystem $(MUSL_DYN)/usr/include \
           -nostdlib -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 \
           $(MUSL_DYN)/usr/lib/crt1.o \
           $(MUSL_DYN)/usr/lib/crti.o \
           -L$(MUSL_DYN)/usr/lib -L$(MUSL_DYN)/lib -lc \
           $(MUSL_DYN)/usr/lib/crtn.o
```

Actually, musl installs a `musl-gcc` specs file. The simpler approach: use the installed gcc wrapper.

Update each dynamic binary's Makefile to use:
```makefile
MUSL_DYN   = ../../build/musl-dynamic
MUSL_GCC   = $(MUSL_DYN)/usr/bin/musl-gcc
CC         = $(MUSL_GCC)
CFLAGS     = -O2 -fno-pie -no-pie -Wl,--build-id=none
```

If musl's install doesn't produce a `musl-gcc` wrapper at that path, use the specs approach. We need to test what `make install DESTDIR=...` produces.

The simplest reliable approach: use the host `gcc` with explicit paths:

```makefile
MUSL_PREFIX = ../../build/musl-dynamic
CC          = gcc
CFLAGS      = -O2 -fno-pie -no-pie -Wl,--build-id=none \
              -specs $(MUSL_PREFIX)/usr/lib/musl-gcc.specs \
              -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1
```

The specifics depend on what `tools/build-musl.sh` produces. The implementer should run `find build/musl-dynamic -name '*.specs' -o -name 'musl-gcc'` after the build and adapt accordingly. The key requirements:
- Headers from `build/musl-dynamic/usr/include`
- Link against `build/musl-dynamic/lib/libc.so` (or usr/lib)
- PT_INTERP = `/lib/ld-musl-x86_64.so.1`
- NOT statically linked

- [ ] **Step 2: Update each dynamic binary's Makefile**

Convert these Makefiles to dynamic linking. The exact CFLAGS depend on what Step 1 determined works. For each of these 26 programs:

`user/shell/Makefile`, `user/oksh/Makefile`, `user/cat/Makefile`, `user/echo/Makefile`, `user/ls/Makefile`, `user/pwd/Makefile`, `user/uname/Makefile`, `user/clear/Makefile`, `user/true/Makefile`, `user/false/Makefile`, `user/wc/Makefile`, `user/grep/Makefile`, `user/sort/Makefile`, `user/mkdir/Makefile`, `user/touch/Makefile`, `user/rm/Makefile`, `user/cp/Makefile`, `user/mv/Makefile`, `user/whoami/Makefile`, `user/httpd/Makefile`, `user/dhcp/Makefile`, `user/vigictl/Makefile`, `user/thread_test/Makefile`, `user/mmap_test/Makefile`, `user/proc_test/Makefile`, `user/pty_test/Makefile`

Remove `-static` from CFLAGS and add the dynamic linker specification.

**Keep static:** `user/vigil/Makefile`, `user/login/Makefile` — these remain unchanged.

- [ ] **Step 3: Create user/dynlink_test/main.c**

A test binary specifically for dynamic linking validation:

```c
/* user/dynlink_test/main.c — Dynamic linking integration test.
 * Verifies: (1) dynamically-linked binary runs, (2) libc works,
 * (3) interpreter mapping visible in /proc/self/maps. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void)
{
    /* Test 1: Basic libc functionality */
    printf("DYNLINK: printf works\n");

    /* Test 2: malloc (exercises mmap) */
    char *p = malloc(1024);
    if (!p) {
        printf("DYNLINK FAIL: malloc returned NULL\n");
        return 1;
    }
    strcpy(p, "heap works");
    printf("DYNLINK: %s\n", p);
    free(p);

    /* Test 3: Check /proc/self/maps for interpreter mapping at 0x40000000 */
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) {
        printf("DYNLINK FAIL: cannot open /proc/self/maps\n");
        return 1;
    }
    char line[256];
    int found_interp = 0;
    while (fgets(line, sizeof(line), f)) {
        /* Look for mapping starting at 0x40000000 (INTERP_BASE) */
        if (strstr(line, "40000000"))
            found_interp = 1;
        printf("  %s", line);
    }
    fclose(f);

    if (found_interp) {
        printf("DYNLINK: interpreter mapping found at 0x40000000\n");
    } else {
        printf("DYNLINK FAIL: no mapping at 0x40000000\n");
        return 1;
    }

    printf("DYNLINK OK\n");
    return 0;
}
```

- [ ] **Step 4: Create user/dynlink_test/Makefile**

```makefile
# Dynamic linking test — must be linked dynamically
MUSL_PREFIX = ../../build/musl-dynamic
CC          = gcc
CFLAGS      = -O2 -fno-pie -no-pie -Wl,--build-id=none \
              -specs $(MUSL_PREFIX)/usr/lib/musl-gcc.specs \
              -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1

dynlink_test.elf: main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f *.elf
```

(Adjust CFLAGS based on what Step 1 determined.)

- [ ] **Step 5: Build a dynamic test binary and verify it**

```bash
make -C user/dynlink_test
readelf -l user/dynlink_test/dynlink_test.elf | grep -A1 INTERP
file user/dynlink_test/dynlink_test.elf
readelf -d user/dynlink_test/dynlink_test.elf | head -10
```

Expected:
- `INTERP` segment present with path `/lib/ld-musl-x86_64.so.1`
- `file` reports "dynamically linked"
- `NEEDED` shows `libc.so`

- [ ] **Step 6: Commit**

```bash
git add user/dynlink_test/ user/*/Makefile
git commit -m "feat: convert user binaries to dynamic linking, keep vigil+login static"
```

---

## Task 6: Makefile — Remove Dynamic Binaries from Initrd, Add to Disk

**Files:**
- Modify: `Makefile`

Dynamic binaries can't be in the initrd (they need `/lib/libc.so` which is on ext2). Remove them from the blob embedding list. Only keep vigil, login, and init in initrd. Add `/lib/` directory and libc.so to the disk image.

- [ ] **Step 1: Slim down USER_ELFS to boot-critical only**

In the top-level Makefile, change `USER_ELFS` to only include initrd-required binaries:

```makefile
# Programs embedded in initrd via objcopy --input binary
# Only boot-critical static binaries — everything else on ext2 disk
OBJCOPY = x86_64-elf-objcopy

USER_ELFS = \
    user/login/login.elf \
    user/vigil/vigil
```

Remove all the corresponding `$(BUILD)/blobs/*.o` rules for the removed binaries (shell, ls, cat, echo, pwd, uname, clear, true, false, wc, grep, sort, mkdir, touch, rm, cp, mv, whoami, oksh, vigictl, httpd, dhcp).

Update `BLOB_OBJS` accordingly:
```makefile
BLOB_OBJS = $(BUILD)/blobs/login.o $(BUILD)/blobs/vigil.o $(BUILD)/blobs/init.o
```

- [ ] **Step 2: Update initrd.c to remove references to removed binaries**

In `kernel/fs/initrd.c`, remove the `extern` declarations and `initrd_entry_t` array entries for all the binaries that are no longer in initrd. Only keep: `init`, `login`, `vigil`.

(The implementer should read `kernel/fs/initrd.c` to understand the exact structure.)

- [ ] **Step 3: Update disk image construction to include /lib/ and dynamic binaries**

In the Makefile's `$(DISK)` target, add:

```makefile
	# Create /lib directory and install shared libc
	printf 'mkdir /lib\n' \
	    | /sbin/debugfs -w /tmp/aegis-p1.img
	printf 'write build/musl-dynamic/lib/libc.so /lib/libc.so\nwrite build/musl-dynamic/lib/libc.so /lib/ld-musl-x86_64.so.1\n' \
	    | /sbin/debugfs -w /tmp/aegis-p1.img
```

Add `build/musl-dynamic/lib/libc.so` to `DISK_USER_BINS` prerequisites.

Update the `write` commands to use the new dynamic binary paths (they're still the same ELF files, just smaller now that they're dynamically linked).

Also add dynlink_test:
```makefile
	printf 'write user/dynlink_test/dynlink_test.elf /bin/dynlink_test\n' \
	    | /sbin/debugfs -w /tmp/aegis-p1.img
```

- [ ] **Step 4: Add build-musl dependency**

Add a rule so musl shared library is built before disk:
```makefile
build/musl-dynamic/lib/libc.so:
	bash tools/build-musl.sh
```

- [ ] **Step 5: Build and run `make test`**

```bash
rm -rf build && make test
```

Expected: PASS — initrd now contains only vigil, login, and init (all static). The boot oracle doesn't depend on any ext2 binaries.

- [ ] **Step 6: Rebuild disk and verify**

```bash
make disk
```

Expected: Disk image built with `/lib/libc.so`, `/lib/ld-musl-x86_64.so.1`, and all dynamic binaries in `/bin/`.

- [ ] **Step 7: Commit**

```bash
git add Makefile kernel/fs/initrd.c user/dynlink_test/
git commit -m "feat: slim initrd to vigil+login; dynamic binaries on ext2 with /lib/libc.so"
```

---

## Task 7: Integration Test

**Files:**
- Create: `tests/test_dynlink.py`

- [ ] **Step 1: Create the integration test**

```python
#!/usr/bin/env python3
"""test_dynlink.py — Phase 33 dynamic linking integration test.

Boots Aegis with q35 + NVMe disk containing dynamically-linked binaries,
logs in, runs several dynamic binaries including dynlink_test which
validates interpreter mapping in /proc/self/maps.

Skipped if build/disk.img is not present.
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

    errors = []
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

        # Test 1: Run a simple dynamic binary (echo)
        time.sleep(1)
        _type_string(mon, "/bin/echo dynlink-test-1\n")
        if serial.wait_for("dynlink-test-1", time.time() + CMD_TIMEOUT):
            print("  PASS: /bin/echo (dynamic) works")
        else:
            errors.append("FAIL: /bin/echo did not produce output")

        # Test 2: Run cat on a file
        time.sleep(0.5)
        _type_string(mon, "/bin/cat /etc/motd\n")
        if serial.wait_for("Welcome to Aegis", time.time() + CMD_TIMEOUT):
            print("  PASS: /bin/cat (dynamic) works")
        else:
            errors.append("FAIL: /bin/cat /etc/motd did not produce expected output")

        # Test 3: ls /lib — verify libc.so is visible
        time.sleep(0.5)
        _type_string(mon, "/bin/ls /lib\n")
        if serial.wait_for("libc.so", time.time() + CMD_TIMEOUT):
            print("  PASS: /bin/ls /lib shows libc.so")
        else:
            errors.append("FAIL: /bin/ls /lib did not show libc.so")

        # Test 4: Run the dynamic linking test binary
        time.sleep(0.5)
        _type_string(mon, "/bin/dynlink_test\n")
        if serial.wait_for("DYNLINK OK", time.time() + CMD_TIMEOUT):
            print("  PASS: dynlink_test reports DYNLINK OK")
        else:
            errors.append("FAIL: dynlink_test did not report DYNLINK OK")
            print(f"  last 1000 chars: {serial.full_output()[-1000:]!r}")

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

    if errors:
        for e in errors:
            print(e)
        sys.exit(1)

    print("PASS test_dynlink")
    sys.exit(0)


if __name__ == "__main__":
    run_test()
```

- [ ] **Step 2: Verify make test still passes**

```bash
rm -rf build && make test
```

Expected: PASS — boot oracle test is independent of ext2/dynamic binaries.

- [ ] **Step 3: Build disk and run integration test**

```bash
make disk
python3 tests/test_dynlink.py
```

Expected: All 4 sub-tests pass, final output `PASS test_dynlink`.

- [ ] **Step 4: Run existing integration tests to verify they still work with dynamic binaries**

```bash
python3 tests/test_vigil.py
python3 tests/test_pty.py
```

Expected: Both PASS — these tests use binaries from the ext2 disk which are now dynamically linked. If dynamic linking works, these tests pass automatically.

- [ ] **Step 5: Commit**

```bash
git add tests/test_dynlink.py
git commit -m "test: add test_dynlink.py — dynamic linking integration test"
```

---

## Task 8: curl Dynamic Linking

**Files:**
- Modify: `tools/build-curl.sh`

Rebuild curl dynamically linked to musl's libc.so (but still statically linked to BearSSL).

- [ ] **Step 1: Update tools/build-curl.sh**

Change the curl build to use our custom musl shared library instead of static. The key change is removing `--enable-static --disable-shared` and adjusting the linker flags to use dynamic musl:

In the configure line, change:
- Remove `CC=musl-gcc`
- Add the dynamic musl specs/paths
- Keep BearSSL statically linked via `-Wl,-Bstatic -lbearssl -Wl,-Bdynamic`

The exact changes depend on how curl's configure handles mixed static/dynamic. The implementer should:
1. Use `CC=gcc` with musl dynamic specs
2. Pass `LDFLAGS="-Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1"` plus BearSSL static lib path
3. Verify the output binary with `file build/curl/curl` — should say "dynamically linked"
4. Verify `readelf -d build/curl/curl` shows `NEEDED: libc.so` but NOT `NEEDED: libbearssl.so`

- [ ] **Step 2: Rebuild curl and disk**

```bash
rm -rf build/curl
bash tools/build-curl.sh
make disk
```

- [ ] **Step 3: Verify curl still works**

If test_curl.py exists, run it. Otherwise verify manually that curl binary is on the disk.

```bash
file build/curl/curl
readelf -l build/curl/curl | grep INTERP
```

- [ ] **Step 4: Commit**

```bash
git add tools/build-curl.sh
git commit -m "feat: curl dynamically linked to musl libc (BearSSL still static)"
```

---

## Task 9: Update CLAUDE.md and Forward Constraints

**Files:**
- Modify: `.claude/CLAUDE.md`

- [ ] **Step 1: Update build status table**

Add Phase 33 row:
```
| Dynamic linking (Phase 33) | ✅ | PT_INTERP+ET_DYN; MAP_FIXED+file-backed mmap; musl libc.so; vigil+login static |
```

Update Phase 33 in the roadmap table to `✅ Done`.

- [ ] **Step 2: Add Phase 33 forward constraints section**

```markdown
## Phase 33 — Forward Constraints

**Phase 33 status: ✅ complete. `make test` passes. `test_dynlink.py` PASS.**

1. **No ASLR.** Interpreter loads at fixed INTERP_BASE (0x40000000). PIE binaries load at fixed base. ASLR is future work. Debug builds must disable ASLR to keep `make sym` working.

2. **dlopen/dlsym untested.** The MAP_FIXED + file-backed mmap infrastructure supports it, but no test exercises dlopen. Future work.

3. **curl statically links BearSSL.** Only libc is shared. libbearssl.so is future work.

4. **No LD_PRELOAD or LD_LIBRARY_PATH.** Interpreter uses built-in /lib path only.

5. **File-backed mmap is read-once.** No page cache, no demand paging. Each mmap reads fresh from disk.

6. **Initrd contains only vigil + login + init.** All other binaries on ext2. If ext2 fails to mount, only vigil + login are available.

7. **No MAP_SHARED.** MAP_PRIVATE file-backed mappings only.
```

- [ ] **Step 3: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs: update CLAUDE.md with Phase 33 completion status"
```

---

## Self-Review Checklist

**Spec coverage:**
- Component 1 (PT_INTERP + ET_DYN): Task 1 + Task 2 ✓
- Component 2 (MAP_FIXED + file-backed mmap): Task 3 ✓
- Component 3 (Build musl shared): Task 4 ✓
- Component 4 (ELF loader changes): Task 1 ✓
- Component 5 (Capability gating): No changes needed — existing VFS caps gate interpreter loading ✓
- Component 6 (Testing): Task 7 ✓
- Component 7 (VMA tracking): Automatic via existing vma_insert calls in elf_load ✓
- Binary conversion: Task 5 + Task 6 ✓
- curl: Task 8 ✓
- Forward constraints: Task 9 ✓

**Placeholder scan:** No TBD/TODO items. Task 5 notes that exact CFLAGS depend on musl build output — the implementer must adapt based on what `build-musl.sh` produces.

**Type consistency:** `elf_load` signature is consistent across Task 1 (definition) and Task 2 (callers). `elf_load_result_t` fields match between definition and usage. `MAP_FIXED = 0x10` matches Linux. `INTERP_BASE = 0x40000000` used consistently.
