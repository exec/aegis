# Aegis Kernel — Claude Code Persistent Instructions

> Read this file completely at the start of every session, every time.
> Do not summarize it. Do not skip sections. Do not assume you remember it.
> If anything here conflicts with a user instruction given in-session,
> follow the in-session instruction and flag the conflict explicitly.

---

## What Aegis Is

Aegis is a clean-slate, capability-based POSIX-compatible kernel. It is not a
Linux fork. It is not a research toy. The goal is a kernel that boots real
x86-64 hardware, runs real software via a musl-compatible ABI, and enforces a
security model where no process — including root — holds ambient authority.

Every design decision must be evaluated against that principle.
If a shortcut undermines capability isolation, the shortcut is wrong.

See MISSION.md for the full mission statement.

---

## Methodology — Superpowers Required

This project uses the Superpowers plugin. Every session must follow this order:

1. **Brainstorm before designing** — invoke `/superpowers:brainstorm` for any
   non-trivial task. Never start designing a subsystem from scratch.
2. **Plan before coding** — invoke `/superpowers:write-plan` after brainstorm.
   Tasks must be ≤5 minutes each. Ambiguous tasks must be split further.
3. **Test before implementing** — RED before GREEN, always.
   Write the failing test first. Watch it fail. Then implement.
   If you write code before a test exists, delete the code and start over.
4. **Review before merging** — invoke the code review skill after each task.
   Critical issues block progress. Do not work around them.
5. **Verify before declaring done** — run `make test` and show the output.
   "This should work" is not evidence. Passing tests are evidence.

If Superpowers is not installed, stop and tell the user before proceeding.

---

## Architecture Rules — Non-Negotiable

### Directory Layout

```
kernel/arch/x86_64/     ← ALL x86-specific code lives here. Nowhere else.
kernel/core/            ← Architecture-agnostic kernel logic only.
                          Pretend it must port to ARM64 tomorrow.
kernel/mm/              ← Memory management (arch-agnostic logic)
kernel/cap/             ← Capability subsystem (Rust)
kernel/fs/              ← VFS and filesystem drivers
kernel/sched/           ← Scheduler
tests/                  ← Test harness and expected output
tools/                  ← Build helpers, QEMU wrappers
docs/                   ← Architecture docs, capability model spec
```

No x86 register names, port I/O, or memory-mapped addresses outside
`kernel/arch/x86_64/`. If you find yourself writing `0xB8000` or `inb/outb`
in `kernel/core/`, stop. You are in the wrong place.

### The C/Rust Boundary

The capability subsystem (`kernel/cap/`) and the syscall validation layer are
written in Rust (`no_std`). Everything else is C.

The boundary is a clean subsystem interface — a set of C-callable functions
with a stable header. It is not sprinkled throughout the codebase. Do not
call Rust from drivers. Do not call Rust from early boot. The boundary is
crossed exactly at syscall entry and capability grant/revoke operations.

Rules for Rust code in this project:
- `no_std` always. No exceptions.
- No `alloc` until the heap is proven working and you have explicit
  sign-off in the session to use it.
- Every `unsafe` block requires a comment of the form:
  `// SAFETY: <reason this is actually safe>`
  A vague comment like `// SAFETY: trust me` is not acceptable.
- FFI functions exposed to C must be marked `#[no_mangle]` and documented.

### Output Routing — The Two Laws

**Law 1:** All kernel output goes through `printk()`. Always.
Never write directly to `0xB8000`. Never write directly to serial port
registers. If you need debug output before `printk` is initialized,
use the raw serial write functions in `serial.c` and leave a comment
explaining why, with a TODO to remove it.

**Law 2:** Serial output must always work, even if VGA fails.
`printk()` writes to serial first, then VGA. If VGA init fails,
serial continues. If serial fails, the system panics immediately —
we are blind without it. Never write code where VGA failure silences
serial output.

---

## Testing Harness — The Definition of Working

A subsystem is not working because it compiles.
A subsystem is not working because it looks right.
A subsystem is working when `make test` exits 0.

### How the harness works

```bash
make test
```

This must:
1. Build the kernel and GRUB-bootable ISO cleanly (exit on compiler warning if -Werror is set)
2. Boot the ISO headless with these exact flags:
   ```
   qemu-system-x86_64 \
     -machine pc \
     -cdrom aegis.iso \
     -boot order=d \
     -display none \
     -vga std \
     -nodefaults \
     -serial stdio \
     -no-reboot \
     -m 128M \
     -device isa-debug-exit,iobase=0xf4,iosize=0x04
   ```
3. Capture COM1 serial output; strip ANSI escape sequences (SeaBIOS and GRUB
   write ANSI codes before the kernel starts); keep only lines starting with `[`
4. Diff against `tests/expected/boot.txt`
5. Exit 0 on exact match, exit 1 on any mismatch

**Why GRUB + ISO instead of `-kernel aegis.elf`:**
QEMU 10 dropped direct ELF64 multiboot2 detection via `-kernel`. It now requires
a PVH ELF Note for 64-bit direct boot. GRUB is the standard multiboot2 loader
and detects our header correctly. The constraint (kernel lines must start with
`[SUBSYSTEM]`) ensures they survive the ANSI-strip filter.

### Serial output format

Every subsystem init must emit exactly one status line to serial:

```
[SUBSYSTEM] OK: <message>
```
or
```
[SUBSYSTEM] FAIL: <reason>
```

Examples:
```
[SERIAL] OK: COM1 initialized at 115200 baud
[VGA] OK: text mode 80x25
[PMM] OK: 126MB usable across 3 regions
[VMM] OK: kernel mapped to 0xFFFFFFFF80000000
[CAP] OK: capability subsystem reserved
[AEGIS] System halted.
```

`tests/expected/boot.txt` must contain the exact lines the kernel will emit,
in order, with no trailing spaces and a single newline at end of file.
When a new subsystem is added, update this file before writing the subsystem.
That is the failing test. Then make it pass.

### make targets

| Target      | What it does                                              |
|-------------|-----------------------------------------------------------|
| `make`      | Build only                                                |
| `make run`  | Build + boot in QEMU with VGA + serial (interactive)     |
| `make test` | Build + boot headless, diff serial output, exit 0 or 1   |
| `make clean`| Remove all build artifacts                                |
| `make docs` | Generate docs if tooling is present (optional)            |

---

## Security Model — Understand This Before Touching cap/

The defining feature of Aegis is capability-based security with no ambient
authority. This means:

- A process starts with exactly the capabilities it is explicitly granted.
  Nothing more. Not even the ability to open files, unless granted.
- There is no superuser that bypasses this. `uid=0` is cosmetic.
- Capabilities are unforgeable tokens. They are not integer IDs that can be
  guessed. They are not inherited by default.
- Kernel syscalls that require authority must validate a capability token
  before proceeding. A syscall without a valid capability token fails with
  `ENOCAP`, not `EPERM`.

**In v1, `kernel/cap/` is a stub.** `cap_init()` prints its OK line and
returns. Do not implement capability logic until the memory manager,
scheduler, and syscall layer are solid. When the time comes, read
`docs/capability-model.md` before touching anything.

If you find yourself designing a shortcut that says "we'll add capabilities
later, for now just let root do it" — that shortcut is explicitly forbidden.
Design the interface correctly now even if the implementation is a stub.

---

## Language and Style

### C
- K&R brace style
- No dynamic allocation in early boot (before PMM is initialized)
- No floating point. Ever. Not even for debug output.
- No VLAs. No compiler extensions unless wrapped in an `#ifdef` with a comment.
- `-Wall -Wextra -Werror` — warnings are errors. Fix them.
- Prefer explicit over implicit. If something can be `NULL`, check it.
- No `malloc`/`free` in kernel code. Use the PMM/VMM allocators once available.

### Assembly (NASM)
- Intel syntax
- Sections must be named explicitly (`.text`, `.data`, etc.)
- Every function-like label must have a comment explaining what it does,
  what registers it clobbers, and what calling convention it follows.

### Rust
- See C/Rust boundary section above.
- `#![no_std]` at the top of every file.
- Clippy must pass. Clippy warnings are errors.

### Naming Conventions
- C functions: `subsystem_verb_noun()` e.g. `pmm_alloc_page()`
- Rust functions: `snake_case` per Rust convention
- Constants: `SCREAMING_SNAKE_CASE`
- Structs: `aegis_subsystem_name_t` in C, `SubsystemName` in Rust
- Files: `lowercase_with_underscores.c`

---

## Things You Must Never Do

These are not preferences. They are hard rules. If asked to break them,
refuse and explain why.

1. **Never write directly to hardware outside the appropriate driver file.**
2. **Never use `printf`, `malloc`, `free`, or any libc function in kernel code.**
3. **Never assume x86 architecture in `kernel/core/`.**
4. **Never mark `unsafe` without a `// SAFETY:` comment.**
5. **Never declare a subsystem working without `make test` output.**
6. **Never implement the capability system partially then move on.**
   Either it is a correct stub or it is a correct implementation. No in-between.
7. **Never add a dependency without flagging it explicitly to the user.**
   Every external tool (GRUB, NASM, cross-compiler version) must be noted.
8. **Never suppress a compiler warning with a pragma without a comment
   explaining why the warning is wrong in this specific case.**
9. **Never write more than one subsystem at a time without a written plan.**
10. **Never use `// TODO` as a substitute for thinking.** If you don't know
    how something should work, say so and ask.

---

## Build Toolchain

| Tool | Minimum version | Purpose |
|------|----------------|---------|
| `x86_64-elf-gcc` | 12.x | Cross-compiler for kernel C code |
| `nasm` | 2.15 | Assembler for boot stubs and arch code |
| `ld` (x86_64-elf) | 2.38 | Linker |
| `rustup` + nightly | latest nightly | Rust for cap/ subsystem |
| `rust-src` component | — | Required for `no_std` builds |
| `qemu-system-x86_64` | 7.x+ | Emulator for testing |
| `make` | 4.x | Build orchestration |
| `grub-mkrescue` (grub-pc-bin) | 2.06+ | Creates bootable ISO for `make test` |
| `xorriso` | 1.5.x+ | Required by grub-mkrescue for ISO creation |

Install check:
```bash
x86_64-elf-gcc --version
nasm --version
rustup show
qemu-system-x86_64 --version
grub-mkrescue --version
xorriso --version
```

If any of these fail, stop and tell the user before writing code.

---

## Build Status — Keep This Updated

Update this section at the end of every session.
A subsystem is ✅ only when `make test` passes with it included.

| Subsystem | Status | Notes |
|-----------|--------|-------|
| Boot entry (multiboot2) | ✅ Done | GRUB loads via multiboot2; identity-mapped at 0x100000 |
| Serial driver (COM1) | ✅ Done | 115200 baud, polling 8N1 |
| VGA text mode driver | ✅ Done | 80×25, ROM font, attr 0x07 |
| printk | ✅ Done | serial-first, VGA-conditional |
| Test harness (make test) | ✅ Done | GRUB ISO + ANSI-strip + diff; exits 0 |
| Physical memory manager | ✅ Done | Bitmap allocator; single-page (4KB) only; multi-page deferred to buddy allocator |
| Virtual memory / paging | ✅ Done | Higher-half kernel at 0xFFFFFFFF80000000; 5-table setup (identity + kernel); identity map kept; teardown deferred to Phase 4 |
| Mapped-window allocator | ✅ Done | VMM_WINDOW_VA=0xFFFFFFFF80600000; phys_to_table/zero_page eliminated from vmm.c; identity map still active (teardown Phase 7) |
| Identity map teardown | ✅ Done | kva bump allocator at pd_hi[4+]; phys casts in sched/proc/elf eliminated; pml4[0] cleared |
| SMAP + user pointer validation | ✅ Done | CR4.SMAP enabled (Broadwell+); sys_write returns EFAULT for kernel addresses; stac/clac per-byte load |
| IDT | ✅ Done | 48 interrupt gates; isr_dispatch handles PIC EOI before calling handlers |
| PIC | ✅ Done | 8259A remapped; IRQ0-15 → vectors 0x20-0x2F; per-driver unmask |
| PIT | ✅ Done | Channel 0 at 100 Hz; arch_get_ticks() accessor; arch_request_shutdown() defers exit to ISR |
| PS/2 keyboard | ✅ Done | Scancode→ASCII; ring buffer; blocking kbd_read() + non-blocking kbd_poll() |
| Scheduler (single-core) | ✅ Done | Preemptive round-robin; circular TCB list; ctx_switch in NASM; sched_start dummy-TCB pattern |
| GDT (ring-3 descriptors) | ✅ Done | User CS 0x23, User DS 0x1B installed; arch_gdt_init() |
| TSS | ✅ Done | RSP0 wired to kernel stack top; arch_tss_init() + arch_set_kernel_stack() |
| SYSCALL/SYSRET | ✅ Done | IA32_STAR/LSTAR/SFMASK; syscall_entry.asm landing pad; o64 sysret |
| ELF loader | ✅ Done | PT_LOAD segments mapped into user PML4 via vmm_map_user_page |
| User process (proc_spawn) | ✅ Done | Per-process PML4; KSTACK_VA; proc_enter_user CR3 switch + iretq |
| Syscall dispatch (C) | ✅ Done | sys_write (fd=1 → printk), sys_exit (→ sched_exit); Phase 5 |
| Capability system (Rust) | ✅ Done | Stub only: cap_init() prints OK line |
| VFS | ⬜ Not started | |
| musl port + shell | ⬜ Not started | |

### Phase 1 deviations from original spec

| Item | Spec | Actual | Reason |
|------|------|--------|--------|
| `x86_64-elf-gcc` | native cross-compiler | symlink → `x86_64-linux-gnu-gcc` 14.2 | Not in Debian repos; functionally equivalent with `-ffreestanding -nostdlib` |
| Boot method | QEMU `-kernel aegis.elf` | GRUB + `-cdrom aegis.iso` | QEMU 10 dropped ELF64 multiboot2 detection via `-kernel`; GRUB detects correctly |
| Serial output isolation | direct `diff` | strip ANSI + `grep ^[` + `diff` | SeaBIOS/GRUB write ANSI sequences to COM1 before kernel; our lines all start with `[` |
| QEMU test flags | `-nographic` | `-display none -vga std` | Without VGA device, GRUB falls back to serial, contaminating COM1 with escape sequences even after ANSI stripping |
---

## Session Startup Checklist

Run through this at the start of every session before touching code:

- [ ] Read MISSION.md
- [ ] Read this file (CLAUDE.md) — all of it
- [ ] Run `make test` — what is the current baseline?
- [ ] Check the Build Status table above
- [ ] Confirm Superpowers is active (`/superpowers:brainstorm` available)
- [ ] Ask the user what the goal for this session is
- [ ] Do not write code until brainstorm and plan are complete

---

### Phase 3 forward-looking constraints

**Identity map teardown and `zero_page()` must be resolved together in Phase 4.**
`vmm.c`'s `zero_page()` works by casting a physical address to a pointer and
writing through it — valid only while the identity window `[0..4MB)` is active.
Phase 4 must provide a **mapped-window allocator** (a fixed virtual window that
always maps the page being initialized) *before* tearing down the identity map.
Tearing down identity first and fixing zero_page second causes a fault you cannot
debug. The order is non-negotiable: mapped-window allocator → tear down identity.

*Last updated: 2026-03-20 — Phase 4 complete, make test GREEN. Preemptive round-robin scheduler live; IDT/PIC/PIT/KBD active; interactive VGA terminal.*

*Last updated: 2026-03-20 — Phase 5 complete, make test GREEN. User-space process runs init binary via SYSCALL/SYSRET; sys_write + sys_exit wired; CR3 save/restore in isr_common_stub; proc_enter_user switches PML4 on KSTACK_VA.*

*Last updated: 2026-03-21 — Phase 6 complete, make test GREEN. Mapped-window allocator live; phys_to_table/zero_page eliminated from vmm.c.*

*Last updated: 2026-03-21 — Phase 7 complete, make test GREEN. Identity map removed; kva allocator live; per-process kernel stacks.*

*Last updated: 2026-03-21 — Phase 8 complete, make test GREEN. SMAP enabled; sys_write validates user pointers; EFAULT returned for kernel addresses.*

### Phase 4 forward-looking constraints

**Identity map still active.** TCB and stack allocations in `sched.c` cast PMM
physical addresses directly to pointers — valid only while the identity window
`[0..4MB)` is live. Phase 5 must provide a mapped-window allocator before
tearing down the identity map (see Phase 3 constraint above — same requirement).

**arch_request_shutdown is a test-harness hook, not a real shutdown path.**
The current implementation writes isa-debug-exit (QEMU only). Phase 5+ must
implement a clean kernel shutdown: drain run queue, flush I/O, then halt.

**Single-core only.** The scheduler has no SMP awareness. `s_current` is a
global with no locking. Multi-core brings here requires per-CPU run queues and
IPI-based preemption — Phase 5+ concern.

### Phase 5 forward-looking constraints

**Identity map still active.** All TCB and kernel-stack allocations use physical
addresses cast directly to pointers — valid only while `[0..4MB)` is identity-
mapped in the master PML4. Phase 6 must introduce a mapped-window allocator
before tearing down the identity map.

**SMAP not enabled.** sys_write dereferences user virtual addresses directly
with no pointer validation.  Phase 6 must add bounds checking
(arg2 + arg3 <= 0x00007FFFFFFFFFFF) and enable SMAP so unintentional
kernel→user dereferences fault.

**Single user process only.** KSTACK_VA (0xFFFFFFFF80400000) is a single fixed
address shared by all user processes. Phase 6 must introduce a VA allocator to
assign a distinct kernel-stack VA per process.

**sched_exit master-PML4 switch.** sched_exit switches to master PML4 at entry
because TCBs live in the identity-mapped [0..4MB) range, which is absent from
user PML4s. This is correct and intentional. Phase 6 cleanup: after introducing
a proper kernel allocator, TCBs should be allocated from the higher-half so
this unconditional switch is no longer necessary.

### Phase 6 constraints — Mapped-window allocator implementation

**Window-map walk pattern: map once, unmap once — not per-level.**
The `vmm_window_map` / `vmm_window_unmap` pair exposes a single fixed virtual
slot backed by `s_window_pte`. The correct walk pattern is:

  map(pml4_phys) → read PML4[i] → overwrite PTE with pdpt_phys → read PDPT[j]
  → overwrite PTE with pd_phys → read PD[k] → overwrite PTE with pt_phys
  → read PT[l] → vmm_window_unmap()

One `invlpg` per level (4 total for a 4-level walk), one unmap at the end.
Do NOT call `vmm_window_unmap()` between levels — that doubles the `invlpg`
count to 8 and creates a window between unmap and re-map where a stale TLB
entry could produce a silent wrong read if any code path (assertion, printk)
executes between the two calls. Overwriting the PTE directly to advance to the
next level is safe: the non-reentrancy guarantee holds because this is
single-threaded at Phase 6 scope.

**`s_window_pte` must remain `volatile` — do not remove it.**
The compiler must not cache the PTE value or reorder the write to `*s_window_pte`
against the `invlpg` instruction. `volatile` prevents the compiler from hoisting
or coalescing the write. The `invlpg` itself is inside an `__asm__ volatile`
block, which acts as a compiler barrier, so write ordering is correct as long as
the `volatile` qualifier stays. Any future maintainer who sees `volatile` on a
static pointer and reaches for the delete key must read this comment first.

**Identity map teardown order is non-negotiable.**
Phase 6 sequence:
  1. Implement `vmm_window_map` / `vmm_window_unmap` (the window slot).
  2. Replace every `phys_to_table()` call in `vmm.c` with window-mapped access.
  3. Verify `make test` is GREEN.
  4. Only then clear PML4[0] (the identity-map entry) and `invlpg` the range.
Tearing down PML4[0] before step 2-3 is complete will cause an undebuggable
fault on the next `phys_to_table()` call.

### Phase 7 forward-looking constraints

**Identity map still active.** Phase 6 eliminated `phys_to_table` from `vmm.c`
but TCBs and kernel stacks in `sched.c` and `proc.c` still cast PMM physical
addresses to pointers via the identity map. Run the following to get a complete
list before starting Phase 7:
```bash
grep -rn '(uint64_t \*)(uintptr_t)\|(uint8_t \*)(uintptr_t)\|(void \*)(uintptr_t)' kernel/
```
Known sites: `sched_spawn` (TCB + stack), `proc_spawn` (PCB + kernel stack).
Phase 7 must migrate these to a higher-half slab or kernel allocator, then
remove the `[0..4MB) → [0..4MB)` identity entries from `pd_lo` and flush TLB.

**Single window slot.** `s_window_pt[512]` has 511 unused entries. Phase 7+
work involving concurrent kernel threads or DMA mappings may require additional
slots.

### Phase 9 forward-looking constraints

**`USER_ADDR_MAX` lives in `syscall.c`.** When `sys_read` or any other
pointer-taking syscall is added, move `USER_ADDR_MAX` to `arch.h` and
`user_ptr_valid` to a shared `kernel/syscall/syscall_util.h` so all syscall
handlers share a single definition. Duplication is a vulnerability.

**Per-character stac/clac is intentionally conservative.** `sys_write` uses
a per-byte `stac`/`clac` window because no kernel scratch buffer exists yet.
Phase 9 must introduce a `copy_from_user(dst_kernel, src_user, len)` helper
that does one `stac`, `memcpy`, `clac` — the standard Linux approach. This
removes the per-iteration overhead and makes the pattern reusable for
`sys_read`.

**kva has no free path.** Every exited process permanently leaks its TCB,
PCB, and stack pages. Phase 9 must introduce a free-list for kva pages and
wire `pmm_free_page` + `vmm_unmap_page` into `sched_exit`. Without this,
sequential process execution exhausts physical memory.
