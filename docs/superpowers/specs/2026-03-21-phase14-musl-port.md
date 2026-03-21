# Phase 14: Minimal musl Port

## Goal

Run a musl-linked static binary on Aegis. Prove the capability stack works end-to-end with a real C runtime: `printf("[INIT] Hello from musl libc!\n")` appears in the serial oracle output.

Seven syscalls are added: three real implementations (`sys_mmap`, `sys_munmap`, `sys_arch_prctl`) and five stubs (`exit_group`, `getpid`, `set_tid_address`, `set_robust_list`, `rt_sigprocmask`). The FS segment base (TLS) is wired via MSR write. A new `user/hello/` binary replaces `user/init/` as the spawned process.

---

## Prerequisites

Phase 13 must be complete and `make test` must be GREEN. Verify:

```bash
grep -n 'CAP_KIND_VFS_READ' kernel/cap/cap.h   # must print a line
grep -n 'sys_brk'           kernel/syscall/syscall.c  # must print lines
make test                                         # must exit 0
```

Also verify `musl-gcc` is available:

```bash
musl-gcc --version
```

If not installed: `apt install musl-tools`. Document in CLAUDE.md toolchain table.

---

## Background

### Why musl needs arch_prctl

musl's `crt1.o` startup code calls `arch_prctl(ARCH_SET_FS, &tcb)` before `main()` to configure the FS segment base register. musl uses FS-relative addressing for all thread-local variables (including `errno`). Without this, any FS-relative dereference faults immediately.

The implementation writes the requested address to IA32_FS_BASE (MSR 0xC0000100) via `wrmsr`. The SYSCALL instruction does not save or restore FS.base, so the value set by `arch_prctl` persists across all subsequent syscalls for the lifetime of the process.

### Why sys_mmap is needed even for a hello world

musl's internal allocator initialises a heap arena by calling `mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0)` on first malloc. `printf` uses `malloc` internally for its format buffer when output exceeds the static FILE buffer. Without `sys_mmap`, the first `malloc` call returns NULL and `printf` silently produces no output or crashes.

### Why mmap_base is a bump allocator in Phase 14

Anonymous mmap regions for a single short-lived process need no VA recycling. The bump allocator starting at `0x0000700000000000` (112 TB) is safely between the heap (low MB range) and the user stack (near 0x7FFFFFFF000). `sys_munmap` frees physical pages but does not reclaim VA. A real range-tracking allocator is Phase 15+ work.

### Why a new user/hello/ binary instead of replacing user/init/

`user/init/` is a raw-syscall reference that proved each kernel primitive in Phases 10–13. Preserving it keeps a working freestanding baseline. `user/hello/` is the musl-linked replacement; `proc_spawn_init` is updated to load `hello_elf` instead of `init_elf`.

### Why no capability gate on sys_mmap

`sys_brk` has no capability gate because a process expanding its own address space does not cross a resource boundary. The same reasoning applies to `sys_mmap(MAP_ANONYMOUS)`: the process is allocating from its own address space using physical memory the PMM already tracks. No shared resource is accessed. No `CAP_KIND_*` constant is introduced.

---

## Architecture

### New field: `kernel/arch/x86_64/arch.h`

Add after the existing `arch_stac` / `arch_clac` static inlines:

```c
/* arch_set_fs_base — write addr to IA32_FS_BASE MSR (0xC0000100).
 * Used by sys_arch_prctl(ARCH_SET_FS) to configure musl's TLS pointer.
 * The SYSCALL instruction does not save/restore FS.base, so this value
 * persists across all syscalls until overwritten. */
static inline void
arch_set_fs_base(uint64_t addr)
{
    uint32_t lo = (uint32_t)addr;
    uint32_t hi = (uint32_t)(addr >> 32);
    __asm__ volatile ("wrmsr" : : "c"(0xC0000100U), "a"(lo), "d"(hi));
}
```

### Changes to `kernel/proc/proc.h`

Add two fields to `aegis_process_t` after `brk`:

```c
typedef struct {
    aegis_task_t  task;
    uint64_t      pml4_phys;
    vfs_file_t    fds[PROC_MAX_FDS];
    cap_slot_t    caps[CAP_TABLE_SIZE];
    uint64_t      brk;        /* heap limit (user VA); grows up */
    uint64_t      mmap_base;  /* next anonymous mmap VA; bump allocator */
    uint64_t      fs_base;    /* FS segment base for TLS; set by arch_prctl */
} aegis_process_t;
```

### Changes to `kernel/proc/proc.c`

**Update extern declarations:** Replace `init_elf` / `init_elf_len` with `hello_elf` / `hello_elf_len`.

**Initialize new fields** after `proc->brk = brk_start`:

```c
/* Initialise mmap bump allocator base. */
proc->mmap_base = 0x0000700000000000ULL;

/* FS base starts at zero; arch_prctl sets it at runtime. */
proc->fs_base   = 0;
```

**Update `proc_spawn_init`:**

```c
void
proc_spawn_init(void)
{
    proc_spawn(hello_elf, (size_t)hello_elf_len);
}
```

### Changes to `kernel/syscall/syscall.c`

#### sys_mmap (syscall 9)

```c
/*
 * sys_mmap — syscall 9
 *
 * Supports MAP_ANONYMOUS | MAP_PRIVATE only.
 * addr must be 0 (kernel chooses VA).
 * prot must be PROT_READ | PROT_WRITE (3).
 * fd must be -1, offset must be 0.
 *
 * Returns mapped VA on success, negative errno on failure.
 * Returns -ENOMEM on OOM (Linux-compatible: MAP_FAILED = (void *)-1).
 * No capability gate — process expands its own address space only.
 */
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_ANONYMOUS   0x20
#define PROT_READ       0x01
#define PROT_WRITE      0x02

static uint64_t
sys_mmap(uint64_t arg1, uint64_t arg2, uint64_t arg3,
         uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    (void)arg6;  /* offset — ignored (must be 0, validated below) */

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
            /* OOM: unmap already-mapped pages and return current base */
            uint64_t v2;
            for (v2 = base; v2 < va; v2 += 4096UL) {
                uint64_t p = vmm_phys_of_user(proc->pml4_phys, v2);
                if (p) {
                    vmm_unmap_user_page(proc->pml4_phys, v2);
                    pmm_free_page(p);
                }
            }
            return (uint64_t)-(int64_t)12;  /* OOM — ENOMEM; musl checks MAP_FAILED */
        }
        vmm_map_user_page(proc->pml4_phys, va, phys,
                          VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE);
    }

    proc->mmap_base += len;
    return base;
}
```

`syscall_dispatch` receives six arguments. Update the dispatch signature and the landing pad in `syscall_entry.asm` to pass arg4, arg5, arg6 (r10, r8, r9 per Linux ABI).

#### sys_munmap (syscall 11)

```c
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
```

#### sys_arch_prctl (syscall 158)

```c
/*
 * sys_arch_prctl — syscall 158
 *
 * arg1 = code
 * arg2 = addr
 *
 * ARCH_SET_FS (0x1002): set FS.base to addr. Writes IA32_FS_BASE MSR
 * and saves to proc->fs_base for future context-switch restore.
 * ARCH_GET_FS (0x1003): write current fs_base to *addr.
 * All other codes: return -EINVAL.
 */
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

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
```

#### Stubs

```c
static uint64_t sys_exit_group(uint64_t arg1) { (void)arg1; sched_exit(); __builtin_unreachable(); }
static uint64_t sys_getpid(void)               { return 1; }
static uint64_t sys_rt_sigprocmask(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
                                               { (void)a;(void)b;(void)c;(void)d; return 0; }
static uint64_t sys_set_tid_address(uint64_t arg1) { (void)arg1; return 1; }
static uint64_t sys_set_robust_list(uint64_t a, uint64_t b) { (void)a;(void)b; return 0; }
```

#### syscall_dispatch — 6-argument signature

`sys_mmap` needs six arguments (the Linux mmap calling convention passes arg6 in r9). The dispatch function and the `syscall_entry.asm` landing pad must be updated to pass a sixth argument.

Current `syscall_dispatch` signature:
```c
uint64_t syscall_dispatch(uint64_t num, uint64_t arg1,
                           uint64_t arg2, uint64_t arg3)
```

New signature:
```c
uint64_t syscall_dispatch(uint64_t num, uint64_t arg1, uint64_t arg2,
                           uint64_t arg3, uint64_t arg4, uint64_t arg5,
                           uint64_t arg6)
```

In `syscall_entry.asm`, the Linux syscall ABI delivers:
- arg1 = rdi, arg2 = rsi, arg3 = rdx, **arg4 = r10**, arg5 = r8, arg6 = r9

The `syscall` instruction clobbers rcx (saves return RIP there), so the kernel uses r10 for arg4 instead of rcx. The SysV C ABI passes the first six arguments in rdi, rsi, rdx, rcx, r8, r9 and the seventh on the stack at [rsp+8] when inside the callee.

Replace the existing Step 3 register shuffle block in `syscall_entry.asm` with the following. The ordering is critical — push r9 before overwriting it, then move r8 before overwriting it:

```nasm
    ; ── Step 3: Linux syscall ABI → SysV C 7-argument calling convention ──────
    ;   Linux: rax=num, rdi=arg1, rsi=arg2, rdx=arg3, r10=arg4, r8=arg5, r9=arg6
    ;   SysV:  rdi=num, rsi=arg1, rdx=arg2, rcx=arg3, r8=arg4,  r9=arg5, [rsp+8]=arg6
    ;
    ;   ORDERING: push r9 (user arg6) BEFORE modifying r9.
    ;             Move r8→r9 (SysV arg5) BEFORE overwriting r8 with r10 (SysV arg4).
    ;             The rdi/rsi/rdx/rcx shuffle is independent of r8/r9/r10.
    push r9          ; arg6 → stack (7th SysV arg; must be at [rsp+8] inside callee)
    mov  r9, r8      ; SysV arg5 ← user r8  (user arg5)
    mov  r8, r10     ; SysV arg4 ← user r10 (user arg4)
    mov  rcx, rdx    ; arg3 ← user rdx
    mov  rdx, rsi    ; arg2 ← user rsi
    mov  rsi, rdi    ; arg1 ← user rdi
    mov  rdi, rax    ; num  ← syscall number

    call syscall_dispatch
    add  rsp, 8      ; discard pushed arg6 before the pop/sysret sequence
```

The `pop r11 / pop rcx / pop rsp / o64 sysret` sequence in Step 4 is unchanged.

Updated dispatch switch:

```c
case  9: return sys_mmap(arg1, arg2, arg3, arg4, arg5, arg6);
case 11: return sys_munmap(arg1, arg2);
case 14: return sys_rt_sigprocmask(arg1, arg2, arg3, arg4);
case 39: return sys_getpid();
case 158: return sys_arch_prctl(arg1, arg2);
case 218: return sys_set_tid_address(arg1);
case 231: return sys_exit_group(arg1);
case 273: return sys_set_robust_list(arg1, arg2);
```

### New directory: `user/hello/`

**`user/hello/main.c`:**

```c
#include <stdio.h>

int main(void)
{
    printf("[INIT] Hello from musl libc!\n");
    return 0;
}
```

**`user/hello/Makefile`:**

```makefile
CC     = musl-gcc
CFLAGS = -static -O2 -fno-pie -no-pie

hello.elf: main.c
	$(CC) $(CFLAGS) -o hello.elf main.c

clean:
	rm -f hello.elf
```

### Changes to `Makefile`

Replace all references to `user/init/init.elf` / `kernel/init_bin.c` / `init_elf` with `user/hello/hello.elf` / `kernel/hello_bin.c` / `hello_elf`:

```makefile
user/hello/hello.elf: user/hello/main.c
	$(MAKE) -C user/hello

kernel/hello_bin.c: user/hello/hello.elf
	cd user/hello && xxd -i hello.elf > ../../kernel/hello_bin.c

$(BUILD)/aegis.elf: kernel/hello_bin.c $(ALL_OBJS) $(CAP_LIB)
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS) $(CAP_LIB)
```

Update `USERSPACE_SRCS` to reference `kernel/hello_bin.c` instead of `kernel/init_bin.c`.

Update `clean` target to clean `user/hello/` instead of `user/init/`.

### Changes to `tests/expected/boot.txt`

Replace `[MOTD] Hello from initrd!` and `[HEAP] OK: brk works` with the single musl output line:

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

---

## Files Changed

| File | Change |
|------|--------|
| `kernel/arch/x86_64/arch.h` | Add `arch_set_fs_base` static inline |
| `kernel/proc/proc.h` | Add `mmap_base` + `fs_base` fields |
| `kernel/proc/proc.c` | Init new fields; update externs to `hello_elf`; update `proc_spawn_init` |
| `kernel/syscall/syscall.c` | Add `sys_mmap`, `sys_munmap`, `sys_arch_prctl`, 5 stubs; extend dispatch to 6 args |
| `kernel/arch/x86_64/syscall_entry.asm` | Pass r10, r8, r9 as arg4/5/6 to `syscall_dispatch` |
| `kernel/syscall/syscall.h` | Update `syscall_dispatch` declaration to 6 args |
| `kernel/hello_bin.c` | New — generated from `user/hello/hello.elf` |
| `Makefile` | Switch from `init_bin.c`/`user/init` to `hello_bin.c`/`user/hello` |
| `user/hello/main.c` | New — musl printf hello world |
| `user/hello/Makefile` | New — `musl-gcc -static` |
| `tests/expected/boot.txt` | Replace `[MOTD]` + `[HEAP]` lines with `[INIT]` line |
| `.claude/CLAUDE.md` | Add `musl-gcc` to toolchain; update build status |

---

## Test Oracle

`make test` exits 0 with the new oracle. Verification audits:

```bash
# musl binary exists and is non-empty
ls -la user/hello/hello.elf

# hello_bin.c generated from hello.elf
grep -n 'hello_elf' kernel/hello_bin.c | head -2

# sys_mmap in dispatch
grep -n 'sys_mmap' kernel/syscall/syscall.c

# arch_prctl in dispatch
grep -n 'arch_prctl' kernel/syscall/syscall.c

# FS base setter in arch.h
grep -n 'arch_set_fs_base' kernel/arch/x86_64/arch.h

# mmap_base field in proc.h
grep -n 'mmap_base' kernel/proc/proc.h

# New binary spawned
grep -n 'hello_elf' kernel/proc/proc.c
```

---

## Success Criteria

1. `make test` exits 0 with new oracle.
2. `[INIT] Hello from musl libc!` appears in serial output.
3. `grep -n 'sys_mmap' kernel/syscall/syscall.c` shows the implementation.
4. `grep -n 'arch_set_fs_base' kernel/arch/x86_64/arch.h` shows the helper.
5. `grep -n 'mmap_base\|fs_base' kernel/proc/proc.h` shows both fields.
6. `musl-gcc --version` succeeds (toolchain check).

---

## Phase 15 Forward-Looking Constraints

**`mmap_base` is a bump allocator.** `sys_munmap` frees physical pages but does not reclaim virtual address space. A process that repeatedly maps and unmaps will exhaust the 112 TB mmap region eventually. A range-tracking VA allocator (interval tree or free-list) is needed before a long-running process or shell is viable.

**No FS.base save/restore on context switch.** Phase 14 has one user process; the FS.base MSR set by `arch_prctl` persists for the process lifetime. When a second user process is introduced, `ctx_switch` must save the outgoing process's FS.base (either RDMSR or from `proc->fs_base`) and restore the incoming process's `fs_base` before returning to user space.

**No `sys_mmap` for file-backed mappings.** `mmap(fd, ...)` returns `-EINVAL`. musl's dynamic linker (`dlopen`) and memory-mapped file I/O both require file-backed mmap. Phase 15+ work.

**No `sys_mprotect`.** musl may call `mprotect` to mark pages read-only (e.g., after relocation). Currently returns `-ENOSYS`. Adding it requires per-page permission tracking in the VMM.

**No `sys_fork` / `sys_exec`.** A shell requires process creation. These are the largest missing syscalls. `fork` requires full address-space cloning (copy-on-write or eager copy of all user PML4 entries). `exec` requires tearing down the current address space and loading a new ELF. Phase 15+ work after musl is stable.
