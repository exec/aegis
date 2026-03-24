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
kernel/drivers/         ← Hardware device drivers (NVMe, xHCI, virtio-net, RTL8125, etc.)
kernel/net/             ← Network stack (netdev_t, Ethernet, IP, TCP, UDP, ICMP)
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
| `musl-gcc` (musl-tools) | 1.2.x+ | Static musl libc cross-compiler for user binaries |
| `sgdisk` (gdisk) | 1.0.x+ | GPT partition table creation for `make disk` |

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

## QEMU Boot Time — Non-Negotiable Invariant

**QEMU must boot to the shell/init prompt in under 30 seconds on this machine.**

If a QEMU session takes more than 30 seconds to reach the first shell prompt,
something is wrong — investigate and fix it before running any tests. Do not
accept, work around, or paper over slow boots.

When a test fails, the debug cycle is: **one failure → diagnose → fix → one retry**.
Do NOT loop retrying the same failing test more than 2 times without changing
something. If a test keeps failing, stop, read the output carefully, use
`make gdb` or serial output to understand why, then fix the root cause.

The `BOOT_TIMEOUT=900` in test scripts is a safety net for loaded CI machines,
not a target. Normal boots are 10–20 seconds. If you see a test waiting more than
60 seconds for a boot prompt, kill QEMU and check what is wrong.

---

## Debug Tooling (added Phase 15 post-fix)

These tools exist to shorten debugging sessions. Use them whenever there
is a kernel panic, unexpected crash, or behavior that needs inspection.

### Compiler flags
CFLAGS includes `-g` (DWARF debug info) and `-fno-omit-frame-pointer`.
`-fno-omit-frame-pointer` ensures every kernel frame has a valid RBP chain
at any optimization level, making stack unwinding reliable.

### Panic backtrace
`isr_dispatch` in `kernel/arch/x86_64/idt.c` calls `panic_backtrace(s->rbp)`
on any kernel-mode exception (CS=0x08). It walks the RBP frame chain and
prints up to 16 return addresses to serial output:
```
[PANIC] backtrace (resolve: make sym ADDR=0x<addr>):
[PANIC]   [0] 0xffffffff80107abc
[PANIC]   [1] 0xffffffff80109def
```
For ring-3 faults (CS=0x23) the backtrace is skipped — the RBP is a
user-space pointer and not meaningful for kernel tracing.

### Address resolution
```bash
make sym ADDR=0xffffffff80107abc
# → copy_from_user at kernel/mm/uaccess.h:23
```
Wraps `addr2line -e build/aegis.elf -f -p`. Requires a build with `-g`.
Use this immediately on any RIP or return address from a panic.

### Interactive GDB session
```bash
make gdb
```
Boots QEMU with `-s -S` (GDB server on :1234, CPU halted at first instruction).
Serial output captured to `build/debug.log`. GDB auto-connects via
`tools/aegis.gdb` with the right arch and symbol file loaded.

In GDB:
- `c` — start the kernel
- `Ctrl-C` — pause at current instruction
- `bt` — backtrace
- `break isr_dispatch` — catch any exception before the panic halt
- `info registers` — all registers
- `x/20gx $rsp` — dump 20 qwords from the stack
- `p *s` — print the full `cpu_state_t` (when stopped inside `isr_dispatch`)

### Workflow for a kernel panic
1. Read the serial output — RIP and CR2 are printed by `isr_dispatch`
2. Run `make sym ADDR=<RIP>` to get the source line
3. Read the backtrace addresses, run `make sym` on each to get the call chain
4. If that's not enough context, `make gdb`, set a breakpoint before the
   faulting code, and step through it interactively

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
| Syscall cleanup + kva free path (Phase 9) | ✅ Done | syscall_util.h + uaccess.h; copy_from_user in sys_write; kva_free_pages wired into sched_exit |
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
| Syscall dispatch (C) | ✅ Done | sys_write cap-gated (VFS_WRITE) + fd-table dispatch; sys_exit/open/read/close |
| Capability system (Rust) | ✅ Done | CapSlot type; cap_grant/cap_check FFI; per-process caps[] table; sys_open + sys_write gated |
| VFS | ✅ Done | vfs.h + initrd.c + console.c; sys_open/read/write/close; fd 1 pre-opened at spawn |
| stdin/stderr/sys_brk (Phase 13) | ✅ Done | kbd VFS (fd 0); stderr (fd 2); sys_brk (syscall 12); CAP_KIND_VFS_READ gate on sys_read; task_kbd retired; task count 2 |
| musl port (Phase 14) | ✅ Done | musl-gcc static binary; sys_mmap anon bump; sys_arch_prctl TLS; sys_writev; SSE init; r8/r9/r10 preserved |
| musl port + shell (Phase 15) | ✅ Done | fork/execve/waitpid; interactive shell; 8 companion programs in initrd |
| Pipes + I/O redirection (Phase 16) | ✅ Done | sys_pipe2/dup/dup2; pipe.c ring buffer; shell pipeline parser; 5/5 smoke tests pass |
| Signals (Phase 17) | ✅ Done | sigaction/sigprocmask/sigreturn/kill/setfg; Ctrl-C kills foreground; iretq+sysret delivery paths; 1/1 smoke test pass |
| stat/getdents64/utilities (Phase 18) | ✅ Done | sys_stat/fstat/lstat/access/nanosleep; getdents64; wc/grep/sort binaries; syscall_entry.asm rdi/rsi/rdx preservation; 4/4 smoke tests pass |
| PCIe enumeration + ACPI (Phase 19) | ✅ Done | MCFG+MADT on q35; graceful skip on -machine pc; kva_alloc_pages ECAM mapping; make test GREEN |
| NVMe driver + blkdev (Phase 20) | ✅ Done | nvme_init on q35; blkdev_register; alloc_queue_page kva window; sfence doorbell; ECAM capped at 8 buses; make test GREEN; test_nvme.py PASS |
| ext2 read-write filesystem (Phase 21) | ✅ Done | ext2 mount on nvme0; 16-slot LRU block cache; read/write/create/unlink/mkdir/rename; sys_mkdir(83)/unlink(87)/rename(82); mkdir/touch/rm/cp/mv user commands; make test GREEN; test_ext2.py PASS |
| xHCI + USB HID keyboard (Phase 22) | ✅ Done | xHCI init on q35+qemu-xhci; USB HID boot protocol; kbd_usb_inject into PS/2 ring buffer; test_xhci.py PASS (init + shell prompt verified) |
| Security audit (post-Phase 22) | ✅ Done | SMEP enabled; sa_handler validation; lseek overflow guards (SEEK_CUR + SEEK_END); ext2 dup ref counting; O_CLOEXEC full lifecycle; SIGPIPE delivery; pipe ring buffer guards; Rust CAP bounds clamp; all tests GREEN |
| GPT partition parsing (Phase 23) | ✅ Done | gpt.c CRC32 + header validation; nvme0p1/nvme0p2 registered; ext2 mounts nvme0p1; test_gpt.py PASS |
| virtio-net driver + netdev_t (Phase 24) | ✅ Done | netdev_t registry; virtio-net eth0; PCI cap walk; KVA BAR map; VIRTIO_F_VERSION_1; 256-slot RX prefill + kick; sfence TX; test_virtio_net.py PASS |

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

*Last updated: 2026-03-21 — Phase 9 complete, make test GREEN. syscall_util.h + uaccess.h introduced; copy_from_user in sys_write; kva_free_pages + deferred cleanup in sched_exit.*

*Last updated: 2026-03-21 — Phase 10 complete, make test GREEN. VFS + sys_read/open/close; static initrd; vmm_free_user_pml4; stack_pages field; task_idle; copy_to_user.*

*Last updated: 2026-03-21 — Phase 11 complete, make test GREEN. CapSlot + cap_grant/cap_check Rust FFI; per-process cap table in aegis_process_t; sys_open gated on CAP_KIND_VFS_OPEN|CAP_RIGHTS_READ; docs/capability-model.md added.*

*Last updated: 2026-03-21 — Phase 12 complete, make test GREEN. Console VFS device; fd 1 pre-opened at spawn; CAP_KIND_VFS_WRITE; sys_write capability-gated + routed through fd table.*

*Last updated: 2026-03-21 — Phase 13 complete, make test GREEN. kbd VFS driver; fd 0/2 pre-open; CAP_KIND_VFS_READ gate on sys_read; sys_brk (syscall 12); task_kbd retired; task count 2.*

*Last updated: 2026-03-21 — Phase 14 complete, make test GREEN. musl-gcc static binary runs via sys_mmap/sys_arch_prctl/sys_writev; SSE init; r8/r9/r10 preserved across syscall; ELF segment alignment fixed; [INIT] Hello from musl libc! confirmed.*

### Phase 13 forward-looking constraints

**`sys_brk` page-aligns the break.** `proc->brk` is always page-aligned after grow or shrink. musl's `malloc` passes exact byte offsets and expects the kernel to return the actual rounded-up break — Phase 13 rounds up, which musl handles correctly.

**fd 0 blocks on `kbd_read()`.** A user process calling `sys_read(0, ...)` will block until a key is pressed. In headless `make test` there is no keyboard input — `init` must not call `sys_read(0, ...)`. Phase 14 or later should provide a `kbd_poll`-based non-blocking path or `sys_poll`.

**No `sys_mmap`.** musl's allocator falls back to `mmap(MAP_ANONYMOUS)` if `brk` fails. Phase 13 provides no `mmap`. A musl port requires either a brk-only allocator config or a minimal `sys_mmap` in Phase 14.

**Capability delegation deferred.** A second user process receives capabilities via `proc_spawn` grants only. `sys_cap_grant` for parent→child delegation remains future work.

### Phase 14 forward-looking constraints

**`mmap_base` is a bump allocator — no free path.** `sys_munmap` is a no-op stub. Pages mapped via `sys_mmap` are never reclaimed. A real allocator (freelist or buddy over the `[0x700000000000, 0x710000000000)` range) is Phase 15+ work.

**`fs_base` is not saved/restored on context switch.** `proc->fs_base` stores the value set by `arch_prctl(ARCH_SET_FS)` but `ctx_switch` in `syscall_entry.asm` does not write IA32_FS_BASE on entry to each task. With a single user process this is correct — the value set at musl startup persists. Multi-process TLS requires saving/restoring IA32_FS_BASE in `ctx_switch` using `wrmsr`/`rdmsr`. Phase 15 must address this before spawning a second user process.

**No file-backed mmap.** `sys_mmap` rejects any call without `MAP_ANONYMOUS` with `-ENOSYS`. File-backed mmap requires VFS integration and a page cache — Phase 15+ work.

**No `mprotect`.** musl does not call `mprotect` in its minimal configuration, but any ELF that marks segments executable post-load will get `-ENOSYS`. Phase 15+ work.

**No `fork`/`exec`.** A shell requires at minimum `sys_fork` + `sys_execve`. Phase 15 (shell) must implement these with full PML4 copy-on-write or a simpler `vfork`+`exec` path.

### Phase 15 forward-looking constraints

**No `sys_munmap` VA reclaim.** Each fork+exec child allocates mmap VA before execve resets it. At Phase 15 scale this is negligible; a real mmap with page freeing is Phase 16 work.

**`sys_chdir` does not validate path existence.** Any string is accepted. Full validation requires a real filesystem in Phase 16.

**No I/O redirection or pipes.** `>`, `<`, `|` are Phase 16.

**No signal handling.** `Ctrl-C` does not kill the foreground process. Phase 16.

**`sys_fork` fd copy without reference counting.** console and kbd close ops are no-ops; safe for Phase 15. Reference counting deferred.

**`opendir`/`readdir` are not generic VFS paths.** The initrd `readdir` is a synthetic listing. A real directory hierarchy with on-disk inodes is Phase 16+.

### Phase 17 forward-looking constraints

**`sys_rt_sigreturn` must NOT restore GS/FS from `gregs[REG_CSGSFS]`.** `signal_deliver` stores only the CS value in `sf.gregs[REG_CSGSFS]` (GS=0, FS=0). When `sys_rt_sigreturn` restores the context, it must set `s->cs` from `gregs[REG_CSGSFS]` and separately call `arch_set_fs_base(proc->fs_base)` to restore musl's TLS pointer. Blindly writing `gregs[REG_CSGSFS]` to the GS/FS registers would zero FS base, corrupting musl thread-local storage on the very first signal return.

**`signal_deliver` is called on every interrupt including ring-0.** The cs check (`s->cs != 0x23`) guards against delivering to kernel context, but the call itself has a small overhead on every PIT tick and keyboard interrupt. If interrupt rate becomes a concern, guard the call in `isr.asm` with a ring-3 check in assembly before the `call signal_deliver`.

**`proc_spawn` kernel stack (4KB) is safe for signal delivery.** Maximum depth at `signal_deliver` entry from RSP0: ~750 bytes (cpu_state_t + signal_deliver frame + 560-byte `rt_sigframe_t` local). 4KB minus 750 bytes leaves ~3350 bytes of headroom, which is adequate.

**Partial GPR restore via sysret sigreturn.** Only rip/rflags/rsp/r8/r9/r10 are in `syscall_frame_t` and restored by `sys_rt_sigreturn`. rbx/rbp/r12-r15/rax/rcx/rdx/rsi/rdi are not restored (they survive through the C call chain for callee-saved regs per SysV ABI). A future phase may add full cpu_state_t on the syscall path.

**No SIGPIPE.** Pipe write with no readers returns -EPIPE (errno) but does not deliver SIGPIPE signal. musl handles -EPIPE gracefully. Add SIGPIPE delivery in Phase 19+.

**No process groups / setpgid.** Ctrl-C delivers to a single foreground PID only (sys_setfg). pgid field deferred to when setpgid/getpgid syscalls are implemented.

**Right Ctrl does not track as Ctrl.** Scancode 0xE0 prefix (right Ctrl, right Alt, etc.) is filtered by `if (sc == 0xE0) return` in kbd_handler. Only left Ctrl (0x1D) is tracked.

### Phase 16 forward-looking constraints

**`O_CLOEXEC` implemented** (security audit post-Phase 22). `sys_open`, `sys_pipe2` propagate the flag; `sys_dup`/`sys_dup2` clear it on the new fd (POSIX); `sys_execve` closes all CLOEXEC fds with full slot zero before loading the new image; `sys_fcntl` F_GETFD/F_SETFD are live.

**`SIGPIPE` implemented** (security audit post-Phase 22). `pipe_write_fn` calls `signal_send_pid(pid, SIGPIPE)` before returning `-EPIPE` when the read end is closed. Kernel tasks with `is_user == 0` receive `-EPIPE` only (no signal).

**Single waiter per pipe end.** `pipe_t` holds one `reader_waiting` and one `writer_waiting`. Multiple concurrent readers or writers on the same pipe end are not supported — the second waiter is never woken. Not a concern for Phase 16 shell pipelines.

**No `sys_munmap` VA reclaim for pipe_t.** `kva_free_pages` is called on pipe close, but the kva VA range is not returned to a free list (bump allocator). Negligible at Phase 16 scale.

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

### Phase 10 forward-looking constraints

**`copy_to_user` not yet needed.** When `sys_read` arrives in Phase 10, add a symmetric
`copy_to_user(dst_user, src_kernel, len)` to `uaccess.h` alongside `copy_from_user`.
Pattern is identical: `arch_stac`, `memcpy`, `arch_clac`.

**`is_user ? 1 : STACK_PAGES` is a hidden contract.** Add a `stack_pages` field to
`aegis_task_t` and populate it at allocation time (`sched_spawn`, `proc_spawn`),
eliminating the inference. A kernel task with a non-`STACK_PAGES` stack size would
silently free the wrong number of pages.

**Last-exit leak.** Simplest fix: permanent idle task. Rename `task_heartbeat` to
`task_idle`, remove the 500-tick exit, and add a separate shutdown mechanism. A
non-exiting kernel task in the run queue ensures every exiting process gets cleaned
up by the deferred pattern at the next call to `sched_exit`.

**User address space teardown deferred.** `sched_exit` still leaks the user PML4
and ELF segment pages. Phase 10 must walk and free PML4 entries 0–255 only (the user
half). Entries 256–511 are the kernel half, shared with the master PML4 — touching
them would corrupt every other process.

*Last updated: 2026-03-21 — Phase 15 complete, make test GREEN. Interactive shell live; fork/execve/waitpid; 8 companion programs in initrd.*

*Last updated: 2026-03-21 — Phase 15 post-fix: three bugs resolved, test_shell.py all 9 commands clean. (1) fork_child_return SYSRET path replaced with isr_post_dispatch iretq path — child's first scheduling now uses complete fake isr_common_stub frame, eliminating r12=0 #PF. (2) arch_set_fs_base now set BEFORE ctx_switch for incoming task in sched_tick/block/yield_to_next. (3) sys_open 256-byte bulk copy replaced with byte-by-byte null-terminated copy — prevents #PF when argv string is within 256 bytes of USER_STACK_TOP (0x7fffffff000). console_write_fn capped to current page boundary.*

*Last updated: 2026-03-21 — Phase 16 complete, test_pipe.py 5/5 GREEN. sys_pipe2/dup/dup2; kernel/fs/pipe.c ring buffer; shell pipeline parser; I/O redirection (<, >, 2>&1). Root cause of test_redirect_stdin flakiness: boot takes 600+ s on loaded host; BOOT_TIMEOUT raised to 900 s and separated from CMD_TIMEOUT (120 s). sys_read gains page-boundary cap matching console_write_fn. test_pipe.py wired into run_tests.sh.*

*Last updated: 2026-03-22 — Phase 18 complete, test_stat.py 4/4 GREEN. sys_stat/fstat/lstat/access/nanosleep/getdents64; wc/grep/sort binaries; syscall_entry.asm now saves/restores user rdi/rsi/rdx across syscall_dispatch (Linux ABI requirement — musl readdir uses rsi after getdents64); sys_brk zeroes new pages. wc -c/-l/-w flag parsing added.*

### Phase 18 forward-looking constraints

**`syscall_entry.asm` rdi/rsi/rdx preservation is a correctness fix, not an optimization.** Any future syscall that passes pointers in rdi/rsi/rdx and has the caller inspect those registers after the syscall returns will now work correctly. This fix is a prerequisite for any musl function that follows the Linux syscall ABI preservation guarantee.

**`sys_stat` field coverage.** `k_stat_t` populates `st_ino`, `st_mode`, `st_size`, `st_nlink`=1, and zeroes uid/gid/dev/rdev/blksize/blocks/timestamps. musl's `stat(3)` works because it only uses `st_size` for most operations. A future phase may populate timestamps from a wall clock when one is available.

**`sys_nanosleep` busy-waits.** The current implementation converts `timespec` to PIT ticks and busy-waits in a loop. This blocks the CPU; the scheduler continues to preempt the process but the process immediately re-enters the busy-wait. A proper `nanosleep` should block the task (sched_block with a wakeup timer) rather than spinning.

**`getdents64` returns synthetic initrd directory.** The directory listing is built directly from `s_files[]` without a real on-disk directory structure. A future VFS redesign with proper directory inodes will replace this.

**No `O_CLOEXEC` on stat-related opens.** File descriptors used internally by stat (via sys_open) are not automatically closed on exec. Deferred alongside full O_CLOEXEC support.

*Last updated: 2026-03-22 — Phase 17 complete, test_signal.py 1/1 GREEN. sigaction/sigprocmask/sigreturn/kill/setfg; iretq and sysret signal delivery paths; Ctrl-C kills foreground process via kbd_handler→signal_send_pid→signal_deliver. Three scheduler/CR3 bugs fixed: sched_block now leaves tasks in run queue so sched_exit can find blocked parents; CR3 restored to user PML4 after ctx_switch returns in sched_block/sched_yield_to_next; signal_deliver temporarily switches to user PML4 for copy_to_user.*

### Phase 19 forward-looking constraints

**ACPI table access is bounded to the first 4MB.** `acpi.c` uses `phys_accessible()` (4MB limit) to guard RSDP/RSDT/XSDT access. On QEMU q35, ACPI tables may be placed above 4MB. A future phase must replace `phys_accessible()` with a `vmm_map_page`-based accessor so real hardware ACPI tables are reachable.

**ECAM MMIO mapped with PWT+PCD flags (no-cache).** The PCIe config space MMIO is mapped via `kva_alloc_pages + vmm_map_page` with flags `0x1B` (Present|Write|PWT|PCD). This is correct for config space registers. BAR memory resources have their own cacheability requirements and must be mapped separately per device.

**`pcie_find_device()` returns the first match only.** If a system has multiple devices of the same class (e.g. two NVMe controllers), only the first is returned. Phase 20 iterates `pcie_get_devices()` directly for the NVMe driver — this is fine for Phase 20 scope.

**MADT is located but not parsed.** `g_madt_found` is set to 1 but MADT entries (LAPIC, IOAPIC, interrupt source override) are not parsed. SMP and APIC-based interrupt routing require MADT parsing — Phase 22+ work.

*Last updated: 2026-03-23 — Phase 19 complete, make test GREEN. ACPI MCFG+MADT parsing; PCIe ECAM enumeration on q35; graceful fallback on -machine pc.*

### Phase 20 forward-looking constraints

**NVMe I/O is synchronous doorbell+poll.** No interrupt-driven completion. A single shared bounce buffer (`s_iobuf`) handles all reads and writes — no concurrent I/O is possible. MSI/MSI-X and interrupt-driven I/O are Phase 22+ work.

**Single namespace only (NSID=1).** Multi-namespace NVMe devices are not enumerated. NSID=1 is hardcoded throughout the driver.

**Transfer size capped at 4096 bytes (one page).** `nvme_blkdev_read` and `nvme_blkdev_write` reject transfers larger than 4096 bytes (`count × 512 > 4096`). PRP list support for multi-page transfers is Phase 21+ work if needed.

**ECAM bus scan capped at 8 buses (`PCIE_MAX_SCAN_BUSES`).** Full 256-bus ECAM mapping would require 256MB of kernel VA — too large for a 128MB test VM. 8 buses = 8MB is sufficient for QEMU q35 and most desktop hardware. A server with many PCIe bridges may need a larger cap.

**Queue memory is never freed.** Admin and I/O queue pages allocated via `alloc_queue_page` are permanent. NVMe hot-remove is not supported.

**GPT partition parsing complete (Phase 23).** `nvme_init` registers `"nvme0"` as a whole-disk blkdev; `gpt_scan("nvme0")` then registers `"nvme0p1"` and `"nvme0p2"` as child blkdevs. The ext2 filesystem mounts from `"nvme0p1"`.

*Last updated: 2026-03-23 — Phase 20 complete, make test GREEN. NVMe 1.4 driver on q35; blkdev abstraction layer; ACPI kva-window for tables above 4MB; ECAM 8-bus cap; test_nvme.py PASS.*

### Phase 21 forward-looking constraints

**No double/triple indirect blocks.** Files larger than ~268KB (12 direct + 256 indirect blocks at 1024 bytes each, or ~4352 blocks at 4096 bytes) cannot be written. Shell scripts and small binaries are fine; large file transfers require double-indirect support.

**Block cache is 16 slots.** Heavy random I/O will thrash the cache. A page cache is v2.0 work.

**No fsck on mount.** Dirty unmount may leave inconsistent state. `fsck.ext2` from host recovers.

**ext2_unlink only frees direct blocks (i_block[0..11]).** Files with data in the single-indirect block (i_block[12]) will have those data blocks leaked when unlinked. Files in Phase 21 shell workloads are small; this leaks only for files >12 blocks (12KB for 1K block size). Fix when adding double-indirect or adding a block-walk unlink.

**No timestamps.** inode atime/mtime/ctime are always 0. A wall clock source (RTC or TSC calibration) is needed for real timestamps.

**New build dependency: `e2tools` (e2mkdir, e2cp) and `debugfs` (from e2fsprogs).** Used by `make disk` to populate the ext2 image. Install: `apt install e2tools e2fsprogs`.

**ext2_vfs_dup_fn uses ref counting.** `ext2_fd_priv_t` carries a `ref_count`; `ext2_vfs_dup_fn` increments it and `ext2_pool_free` decrements, only clearing `in_use` at zero. The previous use-after-free (UAF on dup) was fixed in the security audit pass following Phase 22.

*Last updated: 2026-03-23 — Phase 21 complete, test_ext2.py PASS. ext2 read-write filesystem on nvme0; 16-slot LRU cache; path walk; create/write/unlink/mkdir/rename; 5 user commands; ext2_sync on sched_exit; SMAP-safe write via copy_from_user bounce.*

*Last updated: 2026-03-23 — Phase 22 complete, test_xhci.py PASS. xHCI USB host controller driver; USB HID boot protocol keyboard; kbd_usb_inject into PS/2 ring buffer; [XHCI] OK confirmed on q35; 100Hz polling via PIT.*

*Last updated: 2026-03-23 — Phase 23 complete, test_gpt.py PASS. GPT partition parsing; nvme0p1/nvme0p2 registered; ext2 mounts nvme0p1; make disk rewritten for GPT layout with sgdisk; make test GREEN.*

*Last updated: 2026-03-23 — Phase 24 complete, test_virtio_net.py PASS. netdev_t abstraction; virtio-net eth0 on q35+SLIRP; VIRTIO_F_VERSION_1 negotiated; 256-slot RX prefill; sfence+doorbell TX; make test GREEN (boot oracle unchanged).*

### Phase 22 forward-looking constraints

**Polling-only at 100Hz.** USB HID reports are polled from the PIT tick handler. This adds ~1-2us per tick when no events are pending. MSI-X driven completion is v2.0 work.

**Single keyboard only.** The first HID boot-protocol keyboard found is used. Additional keyboards on other ports are ignored.

**No USB hub support.** Devices must be connected directly to root hub ports. Hub enumeration is v2.0 work.

**No USB mass storage.** Only HID class devices are handled. USB storage class driver is v2.0 work.

**PS/2 and USB coexist.** Both paths push into the same ring buffer via `kbd_usb_inject`. On real hardware without PS/2, only USB HID provides input.

**Transfer ring memory is never freed.** Each device slot's transfer ring is allocated permanently. USB device disconnect does not reclaim resources.

**QEMU usb-kbd Enable Slot timing.** In Phase 22 testing with QEMU's `usb-kbd`, the Enable Slot command may time out during boot (the USB device appears after the kernel scans ports). Razer BlackWidow V3 real hardware testing is deferred — VFIO passthrough via `-device vfio-pci,host=12:00.4` is documented in project memory.

### Security audit forward-looking constraints (post-Phase 22)

**`sys_lseek` SEEK_CUR negative overflow is now guarded.** The audit initially only added the positive overflow guard; the code review identified the missing negative direction. Both guards are now present: `off > 0 && f->offset > INT64_MAX - off` and `off < 0 && f->offset < INT64_MIN - off`. The downstream `new_off < 0` check provides a second defense but relies on UB arithmetic — both guards are required.

**`VFS_O_CLOEXEC` constant replaces bare `0x80000U` literals.** Defined in `vfs.h` alongside `VFS_FD_CLOEXEC`. Both are `0x80000U` by design — same Linux x86-64 value, no shift needed at fd install time. Do not change one without changing the other.

### Phase 23 forward-looking constraints

**Partition LBA bounds are not enforced on reads/writes.** `gpt_part_read` and
`gpt_part_write` add `lba_offset` but do not clamp LBA + count to the partition's
`block_count`. A buggy caller could read past the partition boundary into adjacent
structures. Add bounds checking when the blkdev interface gains a per-device bounds hook.

**`nvme0p2` is registered but not mounted.** It appears as a blkdev but no
filesystem driver uses it. A future phase can format and mount it as swap or a
second ext2 volume.

**Only the primary GPT header is used.** When the primary is valid, the backup is
not checked for consistency. The UEFI spec recommends verifying both headers match;
deferred to a future hardening phase.

**Partition count capped at `GPT_MAX_PARTS = 7`.** With `BLKDEV_MAX = 8` and one
slot for the parent disk. Raise both constants together if more partitions are needed.

**`make disk` rebuilds entirely when any user binary changes.** The `$(DISK)` rule
lists all user ELFs as prerequisites. A change to any binary triggers a full disk
rebuild including running `sgdisk`. This is correct but slow. A future optimization
could separate partition table creation (once) from filesystem population (on change).

### Phase 24 forward-looking constraints

**`netdev_rx_deliver()` is a stub.** Received frames are silently discarded.
Phase 25 replaces this with `eth_rx()` which dispatches based on EtherType
(ARP = 0x0806, IPv4 = 0x0800). Do not add any printk to `netdev_rx_deliver()`
in Phase 24 — SLIRP sends ARP probes and mDNS at boot, which would spam serial.

**Single TX bounce buffer per slot, synchronous poll.** TX is serialized —
one frame in flight at a time, polled to completion before returning. Concurrent
sends from multiple processes share the same `tx_head` counter with no locking.
Correct at Phase 24 (single-threaded kernel). Phase 25+ with a socket layer will
need either a per-process TX lock or a true TX ring with atomic head/tail.

**RX buffer is never freed.** All 256 RX PMM pages (+ 256 TX bounce pages) are
allocated at `virtio_net_init()` and held permanently. `kva_free_pages` is not
called on device reset. Negligible at Phase 24 scale.

**No interrupt-driven RX.** Polling at 100 Hz adds ~1–2µs per tick when no
frames are pending. MSI-X driven RX requires IOAPIC + vector allocation — Phase
25+ or v2.0 work.

**`notify_off_mult = 0` edge case.** Some virtio implementations set
`notify_off_multiplier = 0` to indicate all queues share a single doorbell.
In that case `p->tx_notify_off * 0 / 4 = 0` and the TX doorbell write lands at
`notify_base[0]`, which is the single shared doorbell — correct. QEMU virtio-net
sets the multiplier to 4 (one 32-bit doorbell per queue at offset 0 and 4).

**PCI Enable Bus Master not set.** DMA from the NIC requires PCI command
register bit 2 (Bus Master Enable) to be set. QEMU's firmware (SeaBIOS) sets
this for all devices at boot, so Phase 24 works. On bare metal with UEFI that
does not configure PCIe devices, this bit must be set explicitly via
`pcie_write32(d->bus, d->dev, d->fn, 0x04, pcie_read32(..., 0x04) | 0x06u)`
(Bus Master + Memory Space Enable). Add this to `virtio_net_init()` before
Phase 27 (RTL8125 bare-metal work).

---

## Phase Roadmap (post-Phase 23)

The following phases are planned in order. Framebuffer/display work is intentionally
deferred until the network stack is complete — the server use case takes priority.

| Phase | Content | Notes |
|-------|---------|-------|
| 24 | `netdev_t` abstraction + virtio-net driver | QEMU-testable via `-netdev user` |
| 25 | Ethernet/ARP/IP/ICMP/TCP/UDP protocol stack | Kernel-side; ICMP ping CI test |
| 26 | Full POSIX socket API + epoll | `sys_socket`/`bind`/`listen`/`accept`/`connect`/`send`/`recv`/`epoll_*` |
| 27 | RTL8125 2.5GbE driver (PCI ID 10ec:8125) | Real hardware only; testing deferred until WiFi is set up |
| 28 | DHCP userspace daemon | musl binary; uses socket API; QEMU SLIRP built-in DHCP server for CI |
| 29 | Framebuffer / VESA | Was Phase 23 before network stack inserted |
| 30 | AMD Display Core | Was Phase 24 |
| 31 | Installable disk | Was Phase 25 |
| 32 | Release | Was Phase 26 |

**RTL8125 testing note:** The machine has an RTL8125B 2.5GbE (ASUS, PCI 0a:00.0,
IRQ 24, IOMMU group 18) currently managed by the host `r8169` driver. Testing the
Aegis RTL8125 driver requires either VFIO passthrough or rebooting into Aegis natively.
Do NOT attempt to test the RTL8125 driver until a WiFi connection (MT7921K, 0b:00.0)
is configured and confirmed working, to avoid losing remote access during testing.
