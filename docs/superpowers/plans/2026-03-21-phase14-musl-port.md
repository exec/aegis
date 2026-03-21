# Phase 14: Minimal musl Port — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run a musl-linked static binary on Aegis, proving `printf("[INIT] Hello from musl libc!\n")` reaches serial output through the full capability/VFS/syscall stack.

**Architecture:** Seven new syscalls (sys_mmap with page zeroing, sys_munmap, sys_arch_prctl for TLS, and five stubs); `syscall_dispatch` extended to 7 arguments with a concrete r10/r8/r9 register shuffle in `syscall_entry.asm`; `user/hello/` musl-linked binary replaces `user/init/` as the spawned process.

**Tech Stack:** C (K&R), NASM, musl-gcc (-static -fno-pie -no-pie), existing vmm/pmm/cap/vfs/sched subsystems.

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `tests/expected/boot.txt` | Modify | RED oracle: replace `[MOTD]`+`[HEAP]` with `[INIT]` line |
| `kernel/mm/vmm.h` | Modify | Declare `vmm_zero_page(uint64_t phys)` |
| `kernel/mm/vmm.c` | Modify | Implement `vmm_zero_page` using existing static window helpers |
| `kernel/arch/x86_64/arch.h` | Modify | Add `arch_set_fs_base` static inline (IA32_FS_BASE MSR write) |
| `kernel/proc/proc.h` | Modify | Add `mmap_base` + `fs_base` fields to `aegis_process_t` |
| `kernel/syscall/syscall.h` | Modify | Extend `syscall_dispatch` declaration to 7 arguments |
| `kernel/arch/x86_64/syscall_entry.asm` | Modify | Replace Step 3 with 7-arg register shuffle + `add rsp, 8` |
| `kernel/syscall/syscall.c` | Modify | Add `sys_mmap`, `sys_munmap`, `sys_arch_prctl`, 5 stubs; extend dispatch |
| `user/hello/main.c` | Create | musl printf hello world |
| `user/hello/Makefile` | Create | `musl-gcc -static -fno-pie -no-pie` |
| `Makefile` | Modify | Switch `init_bin.c`→`hello_bin.c`, `user/init`→`user/hello` rules |
| `kernel/proc/proc.c` | Modify | Init `mmap_base`+`fs_base`; switch externs to `hello_elf` |
| `.claude/CLAUDE.md` | Modify | Build status + Phase 14 forward-looking constraints |

---

## Prerequisites

Before starting, verify:

```bash
# Phase 13 must be GREEN
make test   # must exit 0

# musl toolchain must exist
musl-gcc --version
# If missing: apt install musl-tools
```

---

## Task 1: RED — Update oracle

**Files:**
- Modify: `tests/expected/boot.txt`

The oracle is the failing test. Update it *first* so `make test` fails predictably before any implementation.

- [ ] **Step 1: Update `tests/expected/boot.txt`**

Replace the two lines `[MOTD] Hello from initrd!` and `[HEAP] OK: brk works` with the single musl output line. The full file must be exactly:

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
[INIT] Hello from musl libc!
[AEGIS] System halted.
```

No trailing spaces. Single newline at end of file.

- [ ] **Step 2: Verify make test fails**

```bash
make test
```

Expected: FAIL (diff mismatch — `[MOTD]`/`[HEAP]` no longer in oracle, `[INIT]` not yet produced).

- [ ] **Step 3: Commit the failing oracle**

```bash
git add tests/expected/boot.txt
git commit -m "test: Phase 14 RED — replace [MOTD]+[HEAP] oracle with [INIT] musl line"
```

---

## Task 2: vmm_zero_page

**Files:**
- Modify: `kernel/mm/vmm.h`
- Modify: `kernel/mm/vmm.c`

`MAP_ANONYMOUS` must return zeroed pages — Linux guarantee. musl's heap allocator reads free-list metadata from fresh pages; stale PMM data corrupts the allocator. `vmm_window_map`/`vmm_window_unmap` are `static` in `vmm.c`; expose a thin public wrapper.

- [ ] **Step 1: Add declaration to `kernel/mm/vmm.h`**

Add after `vmm_unmap_user_page` (line 82), before `#endif`:

```c
/* vmm_zero_page — zero the physical page at phys using the mapped-window slot.
 * Required for MAP_ANONYMOUS: musl's heap allocator depends on zeroed pages.
 * Uses the internal vmm_window_map/vmm_window_unmap pair. */
void vmm_zero_page(uint64_t phys);
```

- [ ] **Step 2: Add implementation to `kernel/mm/vmm.c`**

Add before `vmm_phys_of_user` (search for `vmm_phys_of_user` to find insertion point):

```c
void
vmm_zero_page(uint64_t phys)
{
    uint8_t *p = (uint8_t *)vmm_window_map(phys);
    int i;
    for (i = 0; i < 4096; i++)
        p[i] = 0;
    vmm_window_unmap();
}
```

Note: no `memset` — kernel has no libc. Same hand-rolled loop pattern as `alloc_table` in this file.

- [ ] **Step 3: Build check**

```bash
make 2>&1 | head -5
```

Expected: clean build (or only pre-existing errors). Fix any `-Werror` issues before continuing.

- [ ] **Step 4: Commit**

```bash
git add kernel/mm/vmm.h kernel/mm/vmm.c
git commit -m "mm: add vmm_zero_page for MAP_ANONYMOUS zeroing requirement"
```

---

## Task 3: arch_set_fs_base

**Files:**
- Modify: `kernel/arch/x86_64/arch.h`

musl's startup calls `arch_prctl(ARCH_SET_FS, &tcb)` to configure FS-relative TLS. This writes IA32_FS_BASE (MSR 0xC0000100) via `wrmsr`. Lives in arch.h as a static inline — same pattern as `arch_stac`/`arch_clac`.

- [ ] **Step 1: Add static inline to `kernel/arch/x86_64/arch.h`**

Add after the `arch_clac` inline (after line 210), before `#endif`:

```c
/* -------------------------------------------------------------------------
 * Phase 14: FS segment base (TLS)
 * ------------------------------------------------------------------------- */

/* arch_set_fs_base — write addr to IA32_FS_BASE MSR (0xC0000100).
 * Used by sys_arch_prctl(ARCH_SET_FS) to configure musl's TLS pointer.
 * The SYSCALL instruction does not save/restore FS.base, so the value
 * set here persists across all subsequent syscalls for the lifetime of
 * the process. When a second user process is introduced, ctx_switch must
 * save proc->fs_base on outgoing and call arch_set_fs_base(proc->fs_base)
 * on incoming before returning to user space. */
static inline void
arch_set_fs_base(uint64_t addr)
{
    uint32_t lo = (uint32_t)addr;
    uint32_t hi = (uint32_t)(addr >> 32);
    __asm__ volatile ("wrmsr" : : "c"(0xC0000100U), "a"(lo), "d"(hi));
}
```

- [ ] **Step 2: Build check**

```bash
make 2>&1 | head -5
```

Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/arch.h
git commit -m "arch: add arch_set_fs_base inline for IA32_FS_BASE MSR write"
```

---

## Task 4: proc.h — mmap_base and fs_base fields

**Files:**
- Modify: `kernel/proc/proc.h`

Add two new fields to `aegis_process_t`.

- [ ] **Step 1: Add fields to `aegis_process_t` in `kernel/proc/proc.h`**

Find the struct definition (currently ends with `uint64_t brk;`). Add two fields after `brk`:

```c
typedef struct {
    aegis_task_t  task;
    uint64_t      pml4_phys;
    vfs_file_t    fds[PROC_MAX_FDS];
    cap_slot_t    caps[CAP_TABLE_SIZE];
    uint64_t      brk;        /* heap limit (user VA); grows up via sys_brk */
    uint64_t      mmap_base;  /* next anonymous mmap VA; bump allocator */
    uint64_t      fs_base;    /* FS segment base for TLS; set by arch_prctl */
} aegis_process_t;
```

- [ ] **Step 2: Build check**

```bash
make 2>&1 | head -5
```

Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add kernel/proc/proc.h
git commit -m "proc: add mmap_base and fs_base fields to aegis_process_t"
```

---

## Task 5: syscall_entry.asm — 7-arg register shuffle

**Files:**
- Modify: `kernel/arch/x86_64/syscall_entry.asm`

Replace Step 3 (the register shuffle before `call syscall_dispatch`) with the 7-argument version. The ordering is critical — registers must be read before they are overwritten.

- [ ] **Step 1: Replace Step 3 in `kernel/arch/x86_64/syscall_entry.asm`**

Find and replace the entire Step 3 block. The old block is:

```nasm
    ; ── Step 3: translate Linux syscall ABI → SysV C calling convention ──────
    ;   Linux: rax=num, rdi=arg1, rsi=arg2, rdx=arg3
    ;   SysV:  rdi=num, rsi=arg1, rdx=arg2, rcx=arg3
    ; rcx already pushed above (return RIP) — safe to overwrite now.
    mov  rcx, rdx    ; arg3 ← user rdx
    mov  rdx, rsi    ; arg2 ← user rsi
    mov  rsi, rdi    ; arg1 ← user rdi
    mov  rdi, rax    ; num  ← syscall number (rax unchanged since SYSCALL)

    call syscall_dispatch
```

Replace with:

```nasm
    ; ── Step 3: Linux syscall ABI → SysV C 7-argument calling convention ──────
    ;   Linux:  rax=num, rdi=arg1, rsi=arg2, rdx=arg3, r10=arg4, r8=arg5, r9=arg6
    ;   SysV 7-arg: rdi=num, rsi=arg1, rdx=arg2, rcx=arg3, r8=arg4, r9=arg5, [rsp+8]=arg6
    ;
    ;   ORDERING: push r9 (user arg6) BEFORE modifying r9.
    ;             Move r8→r9 (SysV arg5) BEFORE overwriting r8 with r10 (SysV arg4).
    ;             The rdi/rsi/rdx/rcx shuffle is independent of r8/r9/r10.
    ;
    ;   Stack layout after push r9 + call (inside syscall_dispatch):
    ;     [rsp+0]  = return address (pushed by call instruction)
    ;     [rsp+8]  = user r9 (arg6) — SysV 7th-argument slot
    ;   After syscall_dispatch returns, add rsp, 8 discards arg6 before pop/sysret.
    push r9          ; arg6 → stack (7th SysV arg; call will make it [rsp+8])
    mov  r9, r8      ; SysV arg5 ← user r8  (user arg5)
    mov  r8, r10     ; SysV arg4 ← user r10 (user arg4; SYSCALL clobbered rcx with RIP)
    mov  rcx, rdx    ; arg3 ← user rdx (safe: rcx was saved to stack in Step 2)
    mov  rdx, rsi    ; arg2 ← user rsi
    mov  rsi, rdi    ; arg1 ← user rdi
    mov  rdi, rax    ; num  ← syscall number (rax unchanged since SYSCALL)

    call syscall_dispatch
    add  rsp, 8      ; discard pushed arg6; restore stack to post-Step-2 layout
```

The `pop r11 / pop rcx / pop rsp / o64 sysret` sequence in Step 4 is unchanged.

- [ ] **Step 2: Build check**

```bash
make 2>&1 | head -10
```

Expected: assembles cleanly (NASM does not validate calling conventions). Linker may still fail due to syscall.c mismatch — that's fixed in Task 7.

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/syscall_entry.asm
git commit -m "arch: extend syscall_entry to pass 7 arguments (r10/r8/r9 as arg4/5/6)"
```

---

## Task 6: syscall.h + syscall.c — extended dispatch and new syscalls

**Files:**
- Modify: `kernel/syscall/syscall.h`
- Modify: `kernel/syscall/syscall.c`

These two files must change atomically — the header declares the 7-arg signature, the implementation provides it.

- [ ] **Step 0: Update `kernel/syscall/syscall.h`**

Replace the current declaration:

```c
uint64_t syscall_dispatch(uint64_t num, uint64_t arg1,
                          uint64_t arg2, uint64_t arg3);
```

With:

```c
/* Called from syscall_entry.asm with IF=0, DF=0, on kernel stack.
 * arg4 = r10 (Linux syscall ABI; r10 used instead of rcx because
 *        SYSCALL clobbers rcx with the return RIP).
 * arg5 = r8, arg6 = r9 per Linux convention.
 * Returns value placed in RAX on syscall return. */
uint64_t syscall_dispatch(uint64_t num, uint64_t arg1, uint64_t arg2,
                          uint64_t arg3, uint64_t arg4, uint64_t arg5,
                          uint64_t arg6);
```

- [ ] **Step 1: Add `#include "arch.h"` to `kernel/syscall/syscall.c`**

Add after the existing includes (after `#include <stdint.h>`):

```c
#include "arch.h"    /* arch_set_fs_base */
```

- [ ] **Step 2: Add MAP/PROT defines and `sys_mmap`**

Add before `syscall_dispatch`, after `sys_brk`:

```c
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
#ifndef MAP_SHARED
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_ANONYMOUS   0x20
#define PROT_READ       0x01
#define PROT_WRITE      0x02
#endif

static uint64_t
sys_mmap(uint64_t arg1, uint64_t arg2, uint64_t arg3,
         uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    (void)arg6;  /* offset — not validated; musl always passes 0 for MAP_ANONYMOUS */

    aegis_process_t *proc = (aegis_process_t *)sched_current();

    /* Only support anonymous private mappings with addr==0. */
    if (arg1 != 0)
        return (uint64_t)-(int64_t)22;   /* EINVAL — fixed addr not supported */
    if (!(arg4 & MAP_ANONYMOUS))
        return (uint64_t)-(int64_t)22;   /* EINVAL — file-backed not supported */
    if (arg4 & MAP_SHARED)
        return (uint64_t)-(int64_t)22;   /* EINVAL — shared not supported */
    if ((arg3 & ~(uint64_t)(PROT_READ | PROT_WRITE)) != 0)
        return (uint64_t)-(int64_t)22;   /* EINVAL — exec/none prot not supported */
    if ((int64_t)arg5 != -1)
        return (uint64_t)-(int64_t)9;    /* EBADF */

    uint64_t len = (arg2 + 4095UL) & ~4095UL;  /* round up to page boundary */
    if (len == 0)
        return (uint64_t)-(int64_t)22;   /* EINVAL */

    uint64_t base = proc->mmap_base;
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
                          VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE);
    }

    proc->mmap_base += len;
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
static uint64_t
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
    return 0;
}

/*
 * sys_arch_prctl — syscall 158
 *
 * arg1 = code
 * arg2 = addr
 *
 * ARCH_SET_FS (0x1002): set FS.base to addr. Writes IA32_FS_BASE MSR
 *   and saves to proc->fs_base.
 * ARCH_GET_FS (0x1003): write current fs_base to *addr.
 * All other codes: return -EINVAL.
 */
#ifndef ARCH_SET_FS
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#endif

static uint64_t
sys_arch_prctl(uint64_t arg1, uint64_t arg2)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();

    if (arg1 == ARCH_SET_FS) {
        proc->fs_base = arg2;
        arch_set_fs_base(arg2);
        return 0;
    }
    if (arg1 == ARCH_GET_FS) {
        if (!user_ptr_valid(arg2, sizeof(uint64_t)))
            return (uint64_t)-(int64_t)14;   /* EFAULT */
        copy_to_user((void *)(uintptr_t)arg2, &proc->fs_base,
                     sizeof(uint64_t));
        return 0;
    }
    return (uint64_t)-(int64_t)22;   /* EINVAL */
}

/* ── Stubs ─────────────────────────────────────────────────────────────────
 * musl startup calls these; they do not require real implementations in
 * Phase 14 with a single short-lived process.
 */
static uint64_t sys_exit_group(uint64_t arg1)
{
    (void)arg1;
    sched_exit();
    __builtin_unreachable();
}

static uint64_t sys_getpid(void) { return 1; }

static uint64_t
sys_rt_sigprocmask(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    (void)a; (void)b; (void)c; (void)d;
    return 0;
}

static uint64_t sys_set_tid_address(uint64_t arg1) { (void)arg1; return 1; }

static uint64_t sys_set_robust_list(uint64_t a, uint64_t b)
{
    (void)a; (void)b;
    return 0;
}
```

- [ ] **Step 3: Update `syscall_dispatch` signature and switch**

Replace the current `syscall_dispatch` function:

```c
uint64_t
syscall_dispatch(uint64_t num, uint64_t arg1,
                 uint64_t arg2, uint64_t arg3)
{
    switch (num) {
    case 0:  return sys_read(arg1, arg2, arg3);
    case 1:  return sys_write(arg1, arg2, arg3);
    case 2:  return sys_open(arg1, arg2, arg3);
    case 3:  return sys_close(arg1);
    case 12: return sys_brk(arg1);
    case 60: return sys_exit(arg1);
    default: return (uint64_t)-1;   /* ENOSYS */
    }
}
```

With:

```c
uint64_t
syscall_dispatch(uint64_t num, uint64_t arg1, uint64_t arg2,
                 uint64_t arg3, uint64_t arg4, uint64_t arg5,
                 uint64_t arg6)
{
    switch (num) {
    case  0: return sys_read(arg1, arg2, arg3);
    case  1: return sys_write(arg1, arg2, arg3);
    case  2: return sys_open(arg1, arg2, arg3);
    case  3: return sys_close(arg1);
    case  9: return sys_mmap(arg1, arg2, arg3, arg4, arg5, arg6);
    case 11: return sys_munmap(arg1, arg2);
    case 12: return sys_brk(arg1);
    case 14: return sys_rt_sigprocmask(arg1, arg2, arg3, arg4);
    case 39: return sys_getpid();
    case 60: return sys_exit(arg1);
    case 158: return sys_arch_prctl(arg1, arg2);
    case 218: return sys_set_tid_address(arg1);
    case 231: return sys_exit_group(arg1);
    case 273: return sys_set_robust_list(arg1, arg2);
    default: return (uint64_t)-1;   /* ENOSYS */
    }
}
```

- [ ] **Step 4: Build check**

```bash
make 2>&1 | head -20
```

Expected: clean build (`make test` still fails — user binary not switched yet).

- [ ] **Step 5: Commit**

```bash
git add kernel/syscall/syscall.h kernel/syscall/syscall.c
git commit -m "syscall: extend dispatch to 7 args; add sys_mmap/munmap/arch_prctl and 5 stubs"
```

---

## Task 7: user/hello/ binary

**Files:**
- Create: `user/hello/main.c`
- Create: `user/hello/Makefile`

Build the musl-linked binary independently. Verify it exists and is a reasonable size before wiring it into the kernel build.

- [ ] **Step 1: Create `user/hello/main.c`**

```c
#include <stdio.h>

int main(void)
{
    printf("[INIT] Hello from musl libc!\n");
    return 0;
}
```

- [ ] **Step 2: Create `user/hello/Makefile`**

```makefile
CC     = musl-gcc
CFLAGS = -static -O2 -fno-pie -no-pie -Wl,--build-id=none

hello.elf: main.c
	$(CC) $(CFLAGS) -o hello.elf main.c

clean:
	rm -f hello.elf
```

`-fno-pie -no-pie`: prevents position-independent code which would require `sys_mmap` for relocation before `main()`. `-Wl,--build-id=none`: prevents build-id section from conflicting with `.text` at the ELF load address (same fix that resolved the LMA overlap in Phase 13 user/init).

- [ ] **Step 3: Build and check size**

```bash
make -C user/hello
ls -lh user/hello/hello.elf
```

Expected: clean build, ELF exists. Check size: if over 2 MB, flag it — the ELF byte array goes into `.data` and inflates the kernel image.

- [ ] **Step 4: Verify ELF is a valid x86-64 static binary**

```bash
file user/hello/hello.elf
```

Expected output: `ELF 64-bit LSB executable, x86-64, ... statically linked`

- [ ] **Step 5: Commit**

```bash
git add user/hello/main.c user/hello/Makefile
git commit -m "user: add musl-linked hello world source (user/hello/)"
```

---

## Task 8: Makefile — switch to hello_bin.c

**Files:**
- Modify: `Makefile`

Wire `user/hello/hello.elf` into the kernel build, replacing `user/init/init.elf`.

- [ ] **Step 1: Update `USERSPACE_SRCS` in `Makefile`**

Find:
```makefile
USERSPACE_SRCS = \
    kernel/syscall/syscall.c \
    kernel/proc/proc.c \
    kernel/elf/elf.c \
    kernel/init_bin.c
```

Replace `kernel/init_bin.c` with `kernel/hello_bin.c`:
```makefile
USERSPACE_SRCS = \
    kernel/syscall/syscall.c \
    kernel/proc/proc.c \
    kernel/elf/elf.c \
    kernel/hello_bin.c
```

- [ ] **Step 2: Replace user/init rules with user/hello rules**

Find and replace the three init-related rules:

```makefile
user/init/init.elf: user/init/main.c
	$(MAKE) -C user/init

kernel/init_bin.c: user/init/init.elf
	cd user/init && xxd -i init.elf > ../../kernel/init_bin.c

$(BUILD)/aegis.elf: kernel/init_bin.c $(ALL_OBJS) $(CAP_LIB)
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS) $(CAP_LIB)
```

With:

```makefile
user/hello/hello.elf: user/hello/main.c
	$(MAKE) -C user/hello

kernel/hello_bin.c: user/hello/hello.elf
	cd user/hello && xxd -i hello.elf > ../../kernel/hello_bin.c

$(BUILD)/aegis.elf: kernel/hello_bin.c $(ALL_OBJS) $(CAP_LIB)
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS) $(CAP_LIB)
```

- [ ] **Step 3: Update clean target**

Find:
```makefile
clean:
	rm -rf $(BUILD)
	rm -f kernel/init_bin.c
	$(MAKE) -C user/init clean
	$(CARGO) clean --manifest-path kernel/cap/Cargo.toml
```

Replace with:
```makefile
clean:
	rm -rf $(BUILD)
	rm -f kernel/init_bin.c kernel/hello_bin.c
	$(MAKE) -C user/init clean
	$(MAKE) -C user/hello clean
	$(CARGO) clean --manifest-path kernel/cap/Cargo.toml
```

(Keep `user/init clean` — user/init is preserved as a freestanding reference.)

- [ ] **Step 4: Build check**

```bash
make 2>&1 | head -20
```

Expected: clean build (will generate `kernel/hello_bin.c` from `hello.elf`). `make test` still fails — proc.c not yet updated.

- [ ] **Step 5: Commit**

```bash
git add Makefile
git commit -m "build: switch kernel ELF embed from init_bin.c to hello_bin.c"
```

---

## Task 9: proc.c — init new fields and switch to hello_elf

**Files:**
- Modify: `kernel/proc/proc.c`

Initialize `mmap_base` and `fs_base` after `brk`, and update the extern declarations and `proc_spawn_init` to use `hello_elf`.

- [ ] **Step 1: Update extern declarations**

Find (lines 17-22 approx):
```c
/*
 * init_elf / init_elf_len — generated by:
 *   cd user/init && xxd -i init.elf > init_bin.c
 * which produces exactly these symbol names when invoked as "xxd -i init.elf".
 */
extern const unsigned char init_elf[];
extern const unsigned int  init_elf_len;
```

Replace with:
```c
/*
 * hello_elf / hello_elf_len — generated by:
 *   cd user/hello && xxd -i hello.elf > ../../kernel/hello_bin.c
 * which produces exactly these symbol names when invoked as "xxd -i hello.elf".
 */
extern const unsigned char hello_elf[];
extern const unsigned int  hello_elf_len;
```

- [ ] **Step 2: Initialize mmap_base and fs_base after brk**

Find:
```c
    /* Initialise heap break to top of ELF segments. */
    proc->brk = brk_start;
```

Add the two new field initializations immediately after:
```c
    /* Initialise heap break to top of ELF segments. */
    proc->brk = brk_start;

    /* Initialise mmap bump allocator base (112 TB — safely above heap, below stack). */
    proc->mmap_base = 0x0000700000000000ULL;

    /* FS base starts at zero; arch_prctl(ARCH_SET_FS) sets it at musl startup. */
    proc->fs_base = 0;
```

- [ ] **Step 3: Update proc_spawn_init**

Find:
```c
void
proc_spawn_init(void)
{
    proc_spawn(init_elf, (size_t)init_elf_len);
}
```

Replace with:
```c
void
proc_spawn_init(void)
{
    proc_spawn(hello_elf, (size_t)hello_elf_len);
}
```

- [ ] **Step 4: Build check**

```bash
make 2>&1 | head -10
```

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add kernel/proc/proc.c
git commit -m "proc: init mmap_base/fs_base fields; switch proc_spawn_init to hello_elf"
```

---

## Task 10: GREEN — make test passes

- [ ] **Step 1: Run make test**

```bash
make test
```

Expected: exits 0 with exact oracle match including `[INIT] Hello from musl libc!`.

If it fails, check the diff output:

```bash
make test 2>&1 | tail -30
```

**Common failure modes:**

| Symptom | Cause | Fix |
|---------|-------|-----|
| `[INIT]` missing, `[MOTD]` appears | `proc_spawn_init` still loading `init_elf` | Re-check Task 10 Step 3 |
| Triple fault / no boot | syscall_entry.asm register shuffle wrong | Check push r9 ordering; verify `add rsp, 8` is after `call` |
| EFAULT on mmap | `arg4`/`arg5` received as 0 in sys_mmap | `r10` not moved to `r8` before call — check shuffle |
| `[INIT]` absent, process exits silently | musl stdout not flushed | Check `exit_group` stub calls `sched_exit()` |
| `make test` FAIL with no serial output | kernel panic during boot | Run `make run` to see VGA output; check `dmesg` in QEMU |

- [ ] **Step 2: Verify key symbols**

```bash
grep -n 'sys_mmap' kernel/syscall/syscall.c | head -3
grep -n 'arch_set_fs_base' kernel/arch/x86_64/arch.h
grep -n 'mmap_base\|fs_base' kernel/proc/proc.h
grep -n 'vmm_zero_page' kernel/mm/vmm.h kernel/mm/vmm.c
grep -n 'hello_elf' kernel/proc/proc.c
ls -lh user/hello/hello.elf
```

All six must produce output.

- [ ] **Step 3: Commit (if not already committed per-task)**

At this point all individual task commits should already be in history. No additional commit needed unless there were fixup commits.

---

## Task 11: CLAUDE.md update

**Files:**
- Modify: `.claude/CLAUDE.md`

- [ ] **Step 1: Update build status table**

Find the `musl port + shell` row:
```
| musl port + shell | ⬜ Not started | |
```

Replace with two rows:
```
| musl port (Phase 14) | ✅ Done | musl-linked hello; sys_mmap/munmap/arch_prctl; 7-arg dispatch |
| shell | ⬜ Not started | |
```

- [ ] **Step 2: Add Phase 14 forward-looking constraints section**

Add after the Phase 13 forward-looking constraints section (search for `**Capability delegation deferred.**`), before `### Phase 4 forward-looking constraints`:

```markdown
### Phase 14 forward-looking constraints

**`mmap_base` is a bump allocator.** `sys_munmap` frees physical pages but does not reclaim virtual address space. A process that repeatedly maps and unmaps will exhaust the 112 TB mmap region eventually. A range-tracking VA allocator (interval tree or free-list) is needed before a long-running process or shell is viable.

**`fs_base` must be saved/restored on context switch when a second user process is introduced.** Phase 14 has one user process; the FS.base MSR set by `arch_prctl` persists for the process lifetime. When a second user process arrives, `ctx_switch` must: (a) save the outgoing process's `fs_base` field (already stored in `proc->fs_base` — no RDMSR needed), (b) call `arch_set_fs_base(incoming->fs_base)` before returning to user space. The restore belongs in the scheduler's context-switch path, not in `syscall_entry.asm`.

**No `sys_mmap` for file-backed mappings.** `mmap(fd, ...)` returns `-EINVAL`. musl's `dlopen` and memory-mapped file I/O both require file-backed mmap. Phase 15+ work.

**No `sys_mprotect`.** musl may call `mprotect` to mark pages read-only after relocation. Currently returns `-ENOSYS`. Adding it requires per-page permission tracking in the VMM.

**No `sys_fork` / `sys_exec`.** A shell requires process creation. `fork` requires full address-space cloning; `exec` requires tearing down the current address space and loading a new ELF. Phase 15+ after musl is stable.
```

- [ ] **Step 3: Add toolchain entry for musl-gcc**

Find the toolchain table and add a row:
```
| `musl-gcc` (musl-tools) | 1.2.x | musl-linked user binaries |
```

- [ ] **Step 4: Add "Last updated" line**

After the most recent Phase 13 "Last updated" line, add:
```
*Last updated: 2026-03-21 — Phase 14 complete, make test GREEN. musl-linked hello binary; sys_mmap (zeroed pages, OOM rollback), sys_munmap, sys_arch_prctl (IA32_FS_BASE), 5 stubs; 7-arg syscall_dispatch.*
```

- [ ] **Step 5: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs: update CLAUDE.md for Phase 14 complete"
```

---

## Success Criteria

1. `make test` exits 0.
2. `[INIT] Hello from musl libc!` appears in serial output.
3. `grep -n 'sys_mmap' kernel/syscall/syscall.c` shows the implementation.
4. `grep -n 'arch_set_fs_base' kernel/arch/x86_64/arch.h` shows the helper.
5. `grep -n 'mmap_base\|fs_base' kernel/proc/proc.h` shows both fields.
6. `grep -n 'vmm_zero_page' kernel/mm/vmm.h` shows the declaration.
7. `musl-gcc --version` succeeds.
