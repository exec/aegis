# Phase 33: Dynamic Linking — Design Spec

## Goal

Add dynamic linking support to Aegis so that user binaries can link against a shared `libc.so` (musl) instead of embedding a full static copy. Reduces disk and memory footprint dramatically. Establishes the kernel infrastructure (PT_INTERP, ET_DYN loading, MAP_FIXED, file-backed mmap) that future phases build on.

## Architecture

musl's `ld-musl-x86_64.so.1` serves as both the ELF interpreter and `libc.so` — they are the same binary. The kernel's job is limited to:

1. Parsing PT_INTERP in the main binary's ELF headers
2. Loading the interpreter into the process's address space
3. Passing the right auxiliary vector (auxv) entries so the interpreter can find the main binary's program headers
4. Setting RIP to the interpreter's entry point instead of the main binary's
5. Supporting MAP_FIXED + file-backed mmap so the interpreter can map additional shared libraries (not needed for libc-only binaries, but required for dlopen and multi-library binaries in future phases)

The kernel knows nothing about relocations, symbol resolution, or dynamic linking internals. That is entirely the interpreter's job.

## Approach: musl's Built-in ldso

musl is unique among C libraries: `ld-musl-x86_64.so.1` and `libc.so` are the same binary. When the kernel loads the interpreter, musl's ldso code self-relocates using only relative addressing (no syscalls), processes the main binary's DT_NEEDED entries, recognizes itself as `libc.so`, resolves all relocations, then jumps to the main binary's `AT_ENTRY`. This is battle-tested across Alpine Linux, Void musl, and other musl-based distributions.

---

## Component 1: Kernel execve — PT_INTERP + ET_DYN

### PT_INTERP handling

When `sys_execve` loads an ELF and finds a PT_INTERP segment:

1. Read the interpreter path from the segment (null-terminated string, e.g., `/lib/ld-musl-x86_64.so.1`).
2. Load the main binary's PT_LOAD segments as today.
3. Open the interpreter via VFS (initrd first, then ext2 — same lookup order as execve).
4. Load the interpreter's PT_LOAD segments into the same process's address space at a fixed base address (`INTERP_BASE = 0x40000000`).
5. Set RIP to the interpreter's entry point (`INTERP_BASE + e_entry` for ET_DYN).
6. Set AT_ENTRY in auxv to the main binary's entry point (not the interpreter's).

### ET_DYN support

The ELF loader currently only accepts ET_EXEC (absolute addresses). Two changes:

- **Interpreter loading:** The interpreter is an ET_DYN binary. Its p_vaddr values are offsets from zero. The kernel adds `INTERP_BASE` to every segment address when mapping. `elf_load` gains a `base` parameter: 0 for ET_EXEC (current behavior), `INTERP_BASE` for ET_DYN interpreters.

- **PIE main binaries:** With `-no-pie` (our default), musl-gcc produces ET_EXEC at 0x400000 — no base offset needed. With `-pie` it produces ET_DYN, which requires a load bias. We accept ET_DYN main binaries and load them at `PIE_BASE = 0x400000` for completeness, but Phase 33 targets ET_EXEC dynamically-linked binaries.

### Extended auxiliary vector

Current auxv entries: AT_PHDR, AT_PHNUM, AT_PAGESZ, AT_ENTRY, AT_RANDOM, AT_NULL.

Add for dynamic linking:
- `AT_BASE` (7) — base address where the interpreter was loaded (`INTERP_BASE`). The interpreter needs this to self-relocate.
- `AT_PHENT` (4) — size of one program header entry (56 bytes for ELF64). musl's ldso uses this to iterate the main binary's PHDRs.

When no PT_INTERP is present (static binary), AT_BASE is 0 and behavior is unchanged.

---

## Component 2: MAP_FIXED + File-Backed mmap

### MAP_FIXED

Current `sys_mmap` rejects `addr != 0`. Changes:

- When `MAP_FIXED` (0x10) is set and `addr != 0`:
  - `addr` must be page-aligned; return EINVAL otherwise.
  - Any valid user VA below `0x800000000000` is accepted.
  - Existing mappings in [addr, addr+len) are silently unmapped first: remove VMA entries, free physical pages, clear PTEs. This unmap-and-replace behavior is required by POSIX and musl's ldso relies on it.
  - Map fresh pages at the exact requested address.
  - Do NOT update the bump allocator (`mmap_base`) — MAP_FIXED operates outside the bump region.

### File-backed mmap (MAP_PRIVATE only)

When `fd != -1` and `MAP_ANONYMOUS` is NOT set:

- `offset` must be page-aligned; return EINVAL otherwise.
- Allocate anonymous pages and zero them (same as MAP_ANONYMOUS).
- Read file contents from `fd` at `offset` into the mapped pages, up to `min(len, file_size - offset)` bytes. Bytes beyond file content remain zero (BSS-like behavior for partial pages).
- The VFS fd must be a regular file (not a pipe, socket, or device). Return ENODEV for non-regular files.
- After mapping, pages are independent of the file (MAP_PRIVATE semantics). Writes do not propagate back to the file. No page cache, no shared mappings.
- VMA type: `VMA_MMAP` (same as anonymous). No need for a separate `VMA_FILE_MMAP` type.

### What we skip

- `MAP_SHARED` — no shared pages between processes.
- Lazy/demand paging — every page allocated eagerly on mmap.
- Page cache — each mmap reads fresh from disk.
- `MAP_FIXED_NOREPLACE` — not needed by musl.

---

## Component 3: Building musl as a Shared Library

### Build script: `tools/build-musl.sh`

- Download musl 1.2.5 source (or use a pinned copy in `references/`).
- Configure: `./configure --prefix=/usr --syslibdir=/lib --target=x86_64-linux-musl`.
- Build: produces `lib/libc.so` (which is also `ld-musl-x86_64.so.1`), `lib/libc.a`, headers, and gcc specs.
- Install to `build/musl-dynamic/` for use by the dynamic build target.

### Two build modes in user/Makefile

- **Static** (`LINK=static`, default for vigil + login): `musl-gcc -static -O2 -fno-pie -no-pie -Wl,--build-id=none` — unchanged from today.
- **Dynamic** (`LINK=dynamic`, default for everything else): Uses the custom-built musl's gcc specs to produce dynamically-linked ET_EXEC binaries with PT_INTERP = `/lib/ld-musl-x86_64.so.1`.

### Binary classification

**Static (initrd, boot-critical) — 2 binaries:**
- `vigil` — PID 1, must run before ext2 is mounted
- `login` — spawned by vigil getty service before ext2 is available

**Dynamic (ext2 disk) — everything else:**
- `shell`, `oksh`, `cat`, `echo`, `ls`, `pwd`, `uname`, `clear`, `true`, `false`, `wc`, `grep`, `sort`, `mkdir`, `touch`, `rm`, `cp`, `mv`, `whoami`, `httpd`, `dhcp`, `vigictl`
- Test binaries: `thread_test`, `mmap_test`, `proc_test`, `pty_test`
- `curl` — dynamically linked to libc, statically linked to BearSSL

### Disk layout

```
/lib/ld-musl-x86_64.so.1    (symlink or copy of libc.so)
/lib/libc.so                  (~800KB)
/bin/cat                      (~20KB, dynamically linked)
/bin/ls                       (~30KB, dynamically linked)
/bin/oksh                     (~150KB, dynamically linked)
...
```

Expected size reduction: each dynamic binary drops from ~800KB to ~20-150KB. One shared ~800KB `libc.so`. Net savings: ~15MB+ across the ext2 image.

---

## Component 4: ELF Loader Changes

### `elf_load` signature change

Current: `int elf_load(const uint8_t *data, uint64_t size, uint64_t pml4_phys, elf_load_result_t *result)`

New: `int elf_load(const uint8_t *data, uint64_t size, uint64_t pml4_phys, uint64_t base, elf_load_result_t *result)`

The `base` parameter is added to every p_vaddr when mapping segments:
- `base = 0` for ET_EXEC (absolute addresses, current behavior)
- `base = INTERP_BASE` for ET_DYN interpreter
- `base = PIE_BASE` for ET_DYN main binary (if PIE support needed)

### ET_DYN acceptance

Currently `elf_load` rejects `e_type != ET_EXEC`. Change to accept both ET_EXEC and ET_DYN:
- ET_EXEC: `base` must be 0 (absolute addressing)
- ET_DYN: `base` is the load bias applied to all segment addresses

### Result struct extension

Add `uint64_t base` to `elf_load_result_t` so the caller knows where the binary was actually loaded (needed for AT_BASE in auxv).

---

## Component 5: Capability Gating

No new capability kind is needed. The interpreter is loaded via VFS open, which already checks `CAP_KIND_VFS_OPEN` and `CAP_KIND_VFS_READ`. A sandboxed process without VFS_READ cannot load shared libraries — dynamic linking fails at interpreter load time with ENOCAP.

Shared library loading by the interpreter (future dlopen) also goes through `sys_open` + `sys_mmap`, both of which are capability-gated.

---

## Component 6: Testing

### `tests/test_dynlink.py`

Integration test, same pattern as test_pty.py:

1. Boot with q35 + NVMe disk image containing dynamically-linked binaries + `/lib/libc.so`.
2. Log in via serial.
3. Wait for DHCP (vigil service).
4. Run `/bin/cat /etc/passwd` — if output appears, the full dynamic linking pipeline works.
5. Run `/bin/ls /lib` — verify `libc.so` and `ld-musl-x86_64.so.1` are visible.
6. Run `/bin/echo hello` — trivial test of a dynamic binary.
7. Run a dynamically-linked test binary that reads `/proc/self/maps` and verifies the interpreter mapping is present at `0x40000000`.

### `make test` (boot oracle)

No changes to `tests/expected/boot.txt`. Dynamic linking is transparent to the kernel's boot sequence — the same subsystem OK lines are emitted. vigil (static) is still PID 1.

### Existing tests

Existing tests (`test_pty.py`, `test_vigil.py`, etc.) continue to work because they use the disk image which will now contain dynamically-linked binaries. If dynamic linking is broken, these tests will fail (the programs won't run), providing broad regression coverage.

---

## Component 7: VMA Tracking for Interpreter

When the kernel loads the interpreter via `elf_load`, VMA entries are recorded automatically (the existing `vma_insert` calls in `elf_load` with `VMA_ELF_TEXT`/`VMA_ELF_DATA`). The interpreter's segments appear in `/proc/self/maps` at `0x40000000+`.

When the interpreter uses `sys_mmap(MAP_FIXED)` to map library segments, those appear as `VMA_MMAP` entries. This is correct — from the kernel's perspective, they're anonymous mappings with file data copied in.

---

## Forward Constraints

1. **dlopen/dlsym deferred.** musl supports dlopen but it requires the interpreter to mmap arbitrary .so files at runtime. The file-backed mmap + MAP_FIXED infrastructure built here is sufficient, but testing dlopen is future work. No additional shared libraries beyond libc.so in Phase 33.

2. **No ASLR.** Interpreter loads at fixed `INTERP_BASE = 0x40000000`. PIE binaries load at fixed `PIE_BASE = 0x400000`. Library base addresses are deterministic. ASLR (randomizing these bases) is future work. When implemented, ASLR will be disabled in debug builds to maintain panic backtrace traceability (`make sym` must continue to resolve addresses deterministically).

3. **curl stays statically linked to BearSSL.** Only libc is shared. Packaging BearSSL as `libbearssl.so` and linking curl dynamically against it is future work.

4. **No lazy binding.** musl does full relocation at load time (equivalent to `LD_BIND_NOW`). No PLT stub optimization needed.

5. **No LD_PRELOAD or LD_LIBRARY_PATH.** Environment variable-driven library search paths are future work. The interpreter finds libc.so via its built-in rpath or the standard `/lib` path.

6. **File-backed mmap is read-once, not demand-paged.** Pages are eagerly allocated and filled from the file at mmap time. No page faults for file reads. No page cache. This is correct but wasteful for large libraries — a page cache is Phase 34+ work.

7. **MAP_SHARED not implemented.** MAP_PRIVATE file-backed mappings only. Shared memory mappings are Phase 39 (IPC) work.

8. **Interpreter path lookup: VFS standard order.** `sys_execve` opens the PT_INTERP path via VFS (initrd fallback → ext2). The interpreter lives on ext2 at `/lib/ld-musl-x86_64.so.1`. If ext2 is not mounted, dynamically-linked binaries cannot start (by design — only static vigil + login run pre-ext2).
