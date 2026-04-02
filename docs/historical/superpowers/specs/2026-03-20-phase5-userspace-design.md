# Phase 5 — User Space Foundation Design

## Goal

Run a statically-linked ELF64 binary in ring 3 under a preemptive scheduler. The binary loops three times printing a message, then calls `exit`. Verified by `make test` diffing serial output against `tests/expected/boot.txt`.

## Scope

Phase 5 delivers the minimum to prove the kernel/user privilege boundary:

- Ring 3 GDT descriptors + TSS + SYSCALL/SYSRET MSRs
- Per-process page tables (one PML4 per process, kernel high entries shared)
- `aegis_process_t` embedding `aegis_task_t` at offset 0
- ELF64 loader for static executables
- Two syscalls: `write` (1) and `exit` (60), Linux-compatible numbers
- A user binary embedded as a C byte array in the kernel image
- `sched_exit()` for task self-removal

**Out of scope for Phase 5 (deferred to Phase 6):**
- Real capability tokens and `cap/` implementation (CLAUDE.md: "do not implement capability logic until the syscall layer is solid")
- Mapped-window allocator / identity map teardown (all Phase 5 allocations stay within the 4MB identity window)
- Process memory cleanup on exit (page tables, stacks leaked — acceptable for a single-shot test binary)
- Multiple user processes or `fork`/`exec`
- `read` syscall, keyboard input from user space

---

## Architecture

### New Directories

```
kernel/arch/x86_64/    — gdt.c/h, tss.c/h, syscall_entry.asm (new files)
kernel/proc/           — proc.h, proc.c
kernel/elf/            — elf.h, elf.c
kernel/syscall/        — syscall.h, syscall.c
user/init/             — main.c, Makefile
```

### Modified Files

```
kernel/arch/x86_64/arch.h  — add arch_gdt_init, arch_tss_init,
                               arch_syscall_init, arch_set_kernel_stack
kernel/sched/sched.h       — add kernel_stack_top + is_user to aegis_task_t;
                               add sched_exit()
kernel/sched/sched.c       — call arch_set_kernel_stack + vmm_switch_to before
                               ctx_switch; add sched_exit(); update sched_spawn
kernel/mm/vmm.h            — add vmm_create_user_pml4, vmm_map_user_page,
                               vmm_switch_to
kernel/mm/vmm.c            — implement above
kernel/core/main.c         — add gdt/tss/syscall init; proc_spawn(init_elf)
Makefile                   — new source dirs; user binary build + embed step
tests/expected/boot.txt    — add GDT/TSS/SYSCALL/USER lines
```

---

## Section 1: GDT / TSS / SYSCALL MSRs

### Runtime GDT (`kernel/arch/x86_64/gdt.c`)

The boot.asm GDT (null + kernel code + kernel data) is replaced at runtime.
`arch_gdt_init()` builds a new static GDT, installs it with `lgdt`, reloads
segment registers, and calls `ltr` to load the TSS selector.

GDT layout:

| Index | Selector | Descriptor     | DPL |
|-------|----------|----------------|-----|
| 0     | `0x00`   | null           | —   |
| 1     | `0x08`   | kernel code L=1| 0   |
| 2     | `0x10`   | kernel data    | 0   |
| 3     | `0x18`   | user data      | 3   |
| 4     | `0x20`   | user code L=1  | 3   |
| 5+6   | `0x28`   | TSS (16 bytes) | 0   |

**CRITICAL ordering constraint**: GDT entries 3 and 4 MUST be user data then
user code in exactly that order. SYSRET derives selectors by arithmetic:
`STAR[63:48]+8 → SS` and `STAR[63:48]+16 → CS`. With `STAR[63:48]=0x10`:
- SS = (0x10+8)|3 = 0x18|3 = 0x1B → GDT[3] = user data ✓
- CS = (0x10+16)|3 = 0x20|3 = 0x23 → GDT[4] = user code ✓

Swapping user data and user code (putting user code at index 3, user data at
index 4) would cause SYSRET to load a data descriptor into CS, triggering an
immediate `#GP` on return to user space. Do not reorder these entries.

SYSCALL/SYSRET MSR values:
- `STAR[47:32] = 0x08` → SYSCALL: CS=0x08 (kernel code), SS=0x10 (kernel data) ✓
- `STAR[63:48] = 0x10` → SYSRET: SS=0x1B (user data), CS=0x23 (user code) ✓

User selectors used in IRETQ frames: CS=0x23, SS=0x1B.

### TSS (`kernel/arch/x86_64/tss.c`)

Static 104-byte `aegis_tss_t` struct. Only RSP0 is used — the CPU loads it
into RSP when an interrupt or exception fires while CPL=3. IOMAP_BASE set to
104 (disables I/O permission bitmap). IST entries zeroed.

```c
void arch_set_kernel_stack(uint64_t rsp0);  /* declared in arch.h */
```

`arch_set_kernel_stack` updates both `tss.rsp0` AND `g_kernel_rsp` (the global
used by the SYSCALL entry stub for its manual stack switch). Both must be kept
in sync — they must always refer to the same value.

### SYSCALL Entry (`kernel/arch/x86_64/syscall_entry.asm`)

MSR programming in `arch_syscall_init()`:
- `IA32_EFER (0xC0000080)` — set bit 0 (SCE) to enable SYSCALL/SYSRET
- `IA32_STAR (0xC0000081)` = `(0x10ULL << 48) | (0x08ULL << 32)`
- `IA32_LSTAR (0xC0000082)` = address of `syscall_entry`
- `IA32_SFMASK (0xC0000084)` = `0x700` (clears IF, TF, DF on entry — DF must
  be clear for kernel C string operations; IF must be clear to prevent nested
  syscall dispatch; TF must be clear to prevent single-step traps)

`syscall_entry` stub sequence:
1. CPU has saved RCX=return RIP, R11=RFLAGS; RSP is still user RSP
2. Load kernel RSP from `g_kernel_rsp` into a scratch register
3. Exchange: save user RSP to `g_user_rsp`, switch to kernel stack
4. Push user RSP (from `g_user_rsp`), RCX (return RIP), R11 (RFLAGS) onto kernel stack
5. Move RAX (syscall number) into RDI; RDI/RSI/RDX already hold args 1-3
   per Linux syscall ABI
6. Call `syscall_dispatch(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3)`
7. Pop R11 (RFLAGS), RCX (return RIP), then `pop rsp` (restores user stack)
8. `sysretq`

`g_kernel_rsp` is a BSS global (single-core; per-CPU extension deferred to Phase 6).
`g_user_rsp` is also a BSS global used transiently during stack switch — it is
not needed after the push to the kernel stack and can be replaced with a
register-exchange approach in a future cleanup. Both globals are updated
atomically with respect to the (single) CPU.

`proc_enter_user` — a bare NASM label in `syscall_entry.asm` (or a dedicated
`proc_entry.asm`), exported with `global proc_enter_user`, containing exactly
one instruction: `iretq`. It must NOT be a C function (C functions have a
prologue/epilogue). In `proc.c`, declare it as `extern void proc_enter_user(void)`
and use it as a function pointer for the initial kernel stack setup only.

### kernel_main init order

```c
arch_gdt_init();    /* [GDT] OK: ring 3 descriptors installed */
arch_tss_init();    /* [TSS] OK: RSP0 initialized             */
arch_syscall_init();/* [SYSCALL] OK: SYSCALL/SYSRET enabled   */
```

These three calls happen after `kbd_init()` and before `sched_init()`.

---

## Section 2: Address Space & Process Type

### VMM Extensions (`kernel/mm/vmm.c`)

```c
/* Allocate a new PML4; copy kernel high entries [256..511] from master.
 * Returns physical address. Valid while identity map is active. */
uint64_t vmm_create_user_pml4(void);

/* Map a single page in the given PML4 (not the active kernel PML4).
 * flags: VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE as needed.
 *
 * CRITICAL: all intermediate page-table entries (PML4e, PDPTe, PDe) created
 * for this mapping must also have VMM_FLAG_USER set. The x86-64 MMU performs
 * a privilege check at every level of the page-table walk — a leaf PTE with
 * USER set but any ancestor without USER will cause a ring-3 #PF even if the
 * leaf mapping is correct. vmm_map_user_page uses an ensure_table_user()
 * helper (not the kernel's ensure_table()) that sets USER|PRESENT|WRITABLE
 * on every intermediate entry it creates. */
void vmm_map_user_page(uint64_t pml4_phys, uint64_t virt,
                       uint64_t phys, uint64_t flags);

/* Load pml4_phys into CR3. Flushes TLB. */
void vmm_switch_to(uint64_t pml4_phys);
```

`vmm_create_user_pml4` copies the master PML4's high 256 entries so the kernel
higher-half is accessible in every user process's address space — required for
syscall handlers to execute without a CR3 switch on SYSCALL entry.

User virtual memory layout (per process):
- `0x400000` — ELF PT_LOAD segments mapped here (standard static ELF base)
- `0x7FFFFFFF000` — user stack top (one 4KB page for Phase 5). `_start` is
  entered via `iretq` with RSP = this value, so RSP must be 16-byte aligned
  per AMD64 ABI. `0x7FFFFFFF000` ends in `0x000` — 4096-byte aligned, hence
  16-byte aligned. ✓ If user code pushes 0 words before its first `call`,
  alignment is maintained.

### `aegis_task_t` Changes

Two fields added to the existing struct in `kernel/sched/sched.h`:

```c
typedef struct aegis_task_t {
    uint64_t             rsp;               /* MUST be first — ctx_switch.asm */
    uint8_t             *stack_base;        /* PMM stack base (for future cleanup) */
    uint64_t             kernel_stack_top;  /* NEW: RSP0 value for TSS */
    uint32_t             tid;
    uint8_t              is_user;           /* NEW: 1 if user process, 0 if kernel task */
    struct aegis_task_t *next;
} aegis_task_t;
```

`rsp` remains at offset 0 — the existing `_Static_assert(offsetof(aegis_task_t, rsp) == 0)`
in `sched.c` enforces this. No other field offset is load-bearing from asm.

`sched_spawn` sets `kernel_stack_top = (uint64_t)(uintptr_t)(stack + STACK_SIZE)`
and `is_user = 0`.

### `aegis_process_t` (`kernel/proc/proc.h`)

```c
/* proc_spawn signature */
void proc_spawn(const uint8_t *elf_data, size_t elf_len);

typedef struct {
    aegis_task_t  task;     /* MUST be first — scheduler casts to aegis_task_t * */
    uint64_t      pml4_phys;
} aegis_process_t;
```

`task.is_user = 1` and `task.kernel_stack_top` are set by `proc_spawn`.
`pml4_phys` is the physical address of the process's PML4 (returned by
`vmm_create_user_pml4`).

### Scheduler Integration

**Authoritative design: `is_user` flag.** The cast approach (reading `pml4_phys`
by casting `aegis_task_t *` to `aegis_process_t *` for kernel tasks) is UB —
kernel tasks are not `aegis_process_t` instances. It must NOT be implemented.

In `sched_tick()`, `sched_start()`, and `sched_exit()`, before `ctx_switch`:

```c
arch_set_kernel_stack(s_current->kernel_stack_top);
if (s_current->is_user)
    vmm_switch_to(((aegis_process_t *)s_current)->pml4_phys);
```

The cast is safe only when `is_user == 1` because `proc_spawn` allocates an
`aegis_process_t` and `task` is at offset 0. `sched.c` must include `proc.h`
for this cast, or `vmm_switch_to` can be called via a function pointer stored
in the task struct (cleaner but deferred to Phase 6). For Phase 5, the include
is acceptable.

### First-Time Ring 3 Entry

`proc_spawn` constructs the user process's kernel stack to look like it was
interrupted while in ring 3. From RSP (lowest address) upward:

```
[r15=0]           ← ctx_switch pops this first (lowest address = current RSP)
[r14=0]
[r13=0]
[r12=0]
[rbp=0]
[rbx=0]           ← ctx_switch pops this last before ret
[proc_enter_user] ← ctx_switch's ret jumps here
[user_entry_rip]  ← iretq pops: RIP (ring-3 entry point)
[0x23]            ← iretq pops: CS  (user code selector | RPL=3)
[0x202]           ← iretq pops: RFLAGS (IF=1, reserved bit 1 set)
[user_stack_top]  ← iretq pops: RSP (top of user stack, 16-byte aligned)
[0x1B]            ← iretq pops: SS  (user data selector | RPL=3)
```

`proc_spawn` builds this in C exactly as `sched_spawn` does for kernel tasks:
decrementing a `uint64_t *` from the kernel stack top. The push order (high
to low) is: SS, user RSP, RFLAGS, CS, user RIP, proc_enter_user, rbx, rbp,
r12, r13, r14, r15. `task.rsp` is set to the address of the r15 slot.

`proc_enter_user` is a bare `iretq` in asm (see Section 1). `ctx_switch`'s
`ret` jumps to it, at which point RSP points to the iretq frame above. `iretq`
pops all five fields and transitions to ring 3 at `user_entry_rip`.

After the first preemption (timer fires while user code runs in ring 3), the
CPU uses TSS.RSP0 to switch to the kernel stack, pushes the standard ring-3
interrupt frame (SS, RSP, RFLAGS, CS, RIP), and the normal
`isr_common_stub → isr_dispatch → sched_tick → ctx_switch → iretq` path
handles all subsequent switches.

---

## Section 3: ELF Loader (`kernel/elf/elf.c`)

```c
/* Load a static ELF64 into pml4_phys. Returns entry virtual address,
 * or 0 on parse error. Panics if PMM is exhausted. */
uint64_t elf_load(uint64_t pml4_phys, const uint8_t *data, size_t len);
```

Algorithm:
1. Verify magic (`\x7fELF`), class (ELFCLASS64=2), machine (EM_X86_64=0x3E),
   type (ET_EXEC=2). Return 0 on any mismatch.
2. For each program header where `p_type == PT_LOAD`:
   a. Allocate `ceil(p_memsz / 4096)` pages from PMM via successive
      `pmm_alloc_page()` calls. **PMM contiguity assumption**: the Phase 3
      bitmap allocator returns physically adjacent frames for sequential calls
      (same assumption as `sched_spawn` — see its comment in `sched.c`). The
      ELF data copy is a single `memcpy` into the first page's physical address.
      If the PMM ever becomes non-sequential (buddy allocator, Phase 6+), this
      must be replaced with a page-by-page copy loop.
   b. Copy `p_filesz` bytes from `data + p_offset` to `(void*)(uintptr_t)first_page_phys`.
   c. Zero bytes `[p_filesz .. p_memsz)` for BSS.
   d. Call `vmm_map_user_page` for each page in `[p_vaddr .. p_vaddr + p_memsz)`,
      mapping to the corresponding physical frame.
      Flags: `VMM_FLAG_PRESENT | VMM_FLAG_USER | (VMM_FLAG_WRITABLE if p_flags & PF_W)`.
3. Return `e_entry` (the ELF entry point virtual address).

No relocations. No dynamic segments. No `.interp` parsing. Unknown segment
types (other than PT_LOAD) are silently skipped — only PT_LOAD is processed.

---

## Section 4: Syscall Dispatch (`kernel/syscall/syscall.c`)

```c
/* Called from syscall_entry.asm with IF=0, DF=0, on kernel stack.
 * Returns value placed in RAX on syscall return. */
uint64_t syscall_dispatch(uint64_t num, uint64_t arg1,
                          uint64_t arg2, uint64_t arg3);
```

Handlers:

**`write` (1)**: `arg1`=fd (ignored for Phase 5), `arg2`=user virtual address
of buffer, `arg3`=length. Direct pointer dereference of `arg2` is safe because
SMAP is not enabled and the user PML4 shares the kernel higher-half. Calls
`printk("%.*s", (int)arg3, (char *)(uintptr_t)arg2)`. Returns `arg3`.

**Phase 5 security debt**: no pointer validation. A user program passing a
kernel-space address as `arg2` would cause `printk` to output arbitrary kernel
memory. Phase 6 must add: (a) bounds check that `arg2 + arg3` is within the
user address range `[0 .. 0x00007FFFFFFFFFFF]`, and (b) SMAP enable so
unintentional kernel→user pointer dereferences fault loudly.

**`exit` (60)**: `arg1`=exit code (ignored for Phase 5). Calls `sched_exit()`.
Does not return.

Unknown syscall numbers: return `(uint64_t)-1` (ENOSYS).

### `sched_exit()` (`kernel/sched/sched.c`)

```c
void sched_exit(void) {
    /* IF=0 throughout (set by IA32_SFMASK on SYSCALL entry) —
     * no preemption can occur during list manipulation. */
    aegis_task_t *prev = s_current;
    while (prev->next != s_current)
        prev = prev->next;

    aegis_task_t *dying  = s_current;
    s_current            = dying->next;
    prev->next           = s_current;
    s_task_count--;

    if (s_current == dying) {   /* last task — system is done */
        arch_request_shutdown();
        for (;;) __asm__ volatile ("hlt");
    }

    arch_set_kernel_stack(s_current->kernel_stack_top);
    if (s_current->is_user)
        vmm_switch_to(((aegis_process_t *)s_current)->pml4_phys);
    ctx_switch(dying, s_current);
    __builtin_unreachable();
}
```

Memory for the exited process (TCB page, kernel stack pages, user PML4 and
descendant tables, ELF segment pages) is leaked intentionally. Phase 6 adds
cleanup via `vmm_destroy_user_pml4` and PMM free.

**Phase 6 cleanup note**: `ctx_switch(dying, s_current)` saves RSP into
`dying->rsp` — which now points into the middle of the kernel stack (the
call frame depth at the time of `sched_exit`). Phase 6's cleanup must use
`dying->task.rsp` to locate the current stack position and
`dying->task.stack_base + STACK_SIZE` to find the allocation top, rather than
freeing `stack_base` directly without accounting for where the stack pointer
currently sits. `stack_base` is the PMM allocation start; RSP is somewhere
above it.

---

## Section 5: User Binary (`user/init/`)

```c
/* user/init/main.c — freestanding, no libc */
static inline long sys_write(int fd, const char *buf, long len) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(1L), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return ret;
}

static inline void sys_exit(int code) {
    __asm__ volatile ("syscall"
        : : "a"(60L), "D"((long)code) : "rcx", "r11");
    __builtin_unreachable();
}

void _start(void) {
    for (int i = 0; i < 3; i++)
        sys_write(1, "[USER] hello from ring 3\n", 25);
    sys_write(1, "[USER] done\n", 12);
    sys_exit(0);
}
```

Compiled with:
```
x86_64-elf-gcc -ffreestanding -nostdlib -nostdinc -static \
  -Wl,-Ttext=0x400000 -Wl,-e,_start -o init.elf main.c
```

**Embedding**: the Makefile runs `xxd` from *within* `user/init/` so the
symbol name is derived from the bare filename, not the path:

```makefile
user/init/init_bin.c: user/init/init.elf
	cd user/init && xxd -i init.elf > init_bin.c
```

This generates exactly:
```c
unsigned char init_elf[] = { 0x7f, 0x45, ... };
unsigned int  init_elf_len = ...;
```

`proc.c` references these with:
```c
extern const unsigned char init_elf[];
extern const unsigned int  init_elf_len;
```

`init_bin.c` must be compiled before any object that includes `proc.h` uses
`init_elf`. The Makefile adds `user/init/init_bin.c` as an explicit
prerequisite of `build/proc/proc.o`.

**Build order**: `user/init/init.elf` → `user/init/init_bin.c` →
`build/proc/proc.o` → link.

---

## Section 6: Test Harness

### Updated `tests/expected/boot.txt`

```
[SERIAL] OK: COM1 initialized at 115200 baud
[VGA] OK: text mode 80x25
[PMM] OK: 127MB usable across 2 regions
[VMM] OK: kernel mapped to 0xFFFFFFFF80000000
[CAP] OK: capability subsystem reserved
[IDT] OK: 48 vectors installed
[PIC] OK: IRQ0-15 remapped to vectors 0x20-0x2F
[PIT] OK: timer at 100 Hz
[KBD] OK: PS/2 keyboard ready
[GDT] OK: ring 3 descriptors installed
[TSS] OK: RSP0 initialized
[SYSCALL] OK: SYSCALL/SYSRET enabled
[SCHED] OK: scheduler started, 3 tasks
[USER] hello from ring 3
[USER] hello from ring 3
[USER] hello from ring 3
[USER] done
[AEGIS] System halted.
```

`[SCHED]` reports 3 tasks (task_kbd, task_heartbeat, init process).

**Output determinism**: `sched_spawn` inserts tasks after `s_current` in the
circular list. After three spawns from a null queue, the list is
`task_kbd → task_heartbeat → init → (back to task_kbd)`. `sched_start`
switches to `task_kbd` first. The round-robin scheduler reaches `init` on the
third slot of the first scheduling round.

Each `sys_write` call runs with IF=0 (cleared by `IA32_SFMASK` on SYSCALL
entry) — the scheduler cannot preempt during syscall dispatch. This means
each individual `write` completes atomically from the scheduler's perspective:
no timer tick can fire mid-`printk` and advance to another task. `init`'s five
syscalls (3× write + 1× write + 1× exit) each complete fully before the next
scheduling opportunity. The `[USER]` lines are therefore always contiguous and
always appear before `[AEGIS]` — not because the loop "completes fast," but
because syscall dispatch is non-preemptible by construction.

### `make test` exit condition

`task_heartbeat` remains unchanged: waits for 500 ticks, prints
`[AEGIS] System halted.`, calls `arch_request_shutdown()`. QEMU exits via
`isa-debug-exit`. The user process will have already completed and removed
itself from the run queue via `sched_exit` by this point.

---

## Identity Map Constraint

All Phase 5 PMM allocations (process TCB, kernel stack, user page tables,
ELF segment pages, user stack page) are made before `sched_start()`. The
Phase 3 bitmap PMM allocates sequentially from just above the kernel image
(~1MB). Estimated Phase 5 allocation: ~150KB (20–30 pages). Total footprint
from boot through Phase 5 setup: well under 4MB. The identity-map constraint
is satisfied; the mapped-window allocator is deferred to Phase 6.

---

## CLAUDE.md Build Status After Phase 5

| Subsystem | Status |
|-----------|--------|
| User space (ring 3) | ✅ Done |
| ELF loader (static) | ✅ Done |
| Syscall dispatch (write, exit) | ✅ Done |
| Capability system | ⬜ Phase 6 |
| Mapped-window allocator | ⬜ Phase 6 |
| VFS | ⬜ Phase 6+ |
| musl port + shell | ⬜ Phase 6+ |
