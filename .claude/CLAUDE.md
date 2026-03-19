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
     -nographic \
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
| Physical memory manager | ⬜ Not started | |
| Virtual memory / paging | ⬜ Not started | |
| Scheduler (single-core) | ⬜ Not started | |
| Syscall dispatch (Rust) | ⬜ Not started | |
| Capability system (Rust) | ✅ Done | Stub only: cap_init() prints OK line |
| VFS | ⬜ Not started | |
| ELF loader | ⬜ Not started | |
| musl port + shell | ⬜ Not started | |

### Phase 1 deviations from original spec

| Item | Spec | Actual | Reason |
|------|------|--------|--------|
| `x86_64-elf-gcc` | native cross-compiler | symlink → `x86_64-linux-gnu-gcc` 14.2 | Not in Debian repos; functionally equivalent with `-ffreestanding -nostdlib` |
| Boot method | QEMU `-kernel aegis.elf` | GRUB + `-cdrom aegis.iso` | QEMU 10 dropped ELF64 multiboot2 detection via `-kernel`; GRUB detects correctly |
| Serial output isolation | direct `diff` | strip ANSI + `grep ^[` + `diff` | SeaBIOS/GRUB write ANSI sequences to COM1 before kernel; our lines all start with `[` |
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

*Last updated: 2026-03-19 — Phase 1 complete, make test GREEN.*
