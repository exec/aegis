# Phase 26: Documentation + v1.0 Release Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Write comprehensive documentation (README, architecture guide, hardware support, contributing guide), add an MIT license, finalize the CLAUDE.md build status table, and tag the v1.0.0 release.

**Architecture:** This phase produces no kernel code changes. It creates five documentation files and updates CLAUDE.md. The release tag is applied after all tests pass and documentation is complete.

**Tech Stack:** Markdown, git tags.

**Spec:** docs/superpowers/specs/2026-03-23-aegis-v1-design.md — Phase 26

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `LICENSE` | Create | MIT license text |
| `README.md` | Create | Project overview, quick-start (QEMU + real hardware), build deps, screenshots |
| `CONTRIBUTING.md` | Create | Architecture overview, coding standards, how to add a driver |
| `docs/architecture.md` | Create | Full subsystem reference, design decisions, directory layout |
| `docs/hardware-support.md` | Create | Tested hardware, supported features, known limitations |
| `.claude/CLAUDE.md` | Modify | Final build status table with all 26 phases marked done |

---

### Task 1: Create LICENSE (MIT)

**Files:**
- Create: `LICENSE`

- [ ] **Step 1: Write MIT license**

```
MIT License

Copyright (c) 2026 Dylan

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

- [ ] **Step 2: Commit**

```bash
git add LICENSE
git commit -m "Add MIT license"
```

---

### Task 2: Write README.md

**Files:**
- Create: `README.md`

- [ ] **Step 1: Write README.md**

The README must cover:

1. **What Aegis is** — one paragraph: clean-slate capability-based POSIX-compatible x86-64 kernel. Not a Linux fork. Boots real hardware.

2. **Quick Start — QEMU** — step-by-step commands:
   ```
   # Install dependencies (Debian/Ubuntu)
   sudo apt install build-essential nasm qemu-system-x86 grub-pc-bin \
       xorriso musl-tools e2fsprogs gdisk e2tools

   # Build and run in QEMU
   make INIT=shell iso
   make run
   ```

3. **Quick Start — Real Hardware** — step-by-step:
   ```
   # Build installable disk image (requires sudo)
   make INIT=shell iso
   sudo make disk

   # Write to USB drive or NVMe (DANGEROUS — erases target)
   make install

   # Or test in QEMU with UEFI
   sudo apt install ovmf grub-efi-amd64-bin
   make run-disk
   ```

4. **Hardware Requirements**:
   - CPU: x86-64 with SSE2 (any modern CPU; SMAP requires Broadwell+)
   - Storage: NVMe M.2 (for disk boot)
   - Display: AMD iGPU (Ryzen 5000G-7040 for native DC output) or any VESA-compatible GPU
   - Keyboard: USB HID or PS/2
   - RAM: 128MB minimum

5. **Build Dependencies** — table of all tools with install commands

6. **Make Targets** — table of `make`, `make run`, `make test`, `make disk`, `make run-disk`, `make install`, `make clean`, `make gdb`

7. **Project Structure** — condensed directory layout

8. **Design Principles** — capability-based security, no ambient authority, serial-first output, TDD methodology

9. **License** — MIT

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "docs: add README.md with quick-start, hardware requirements, build instructions"
```

---

### Task 3: Write CONTRIBUTING.md

**Files:**
- Create: `CONTRIBUTING.md`

- [ ] **Step 1: Write CONTRIBUTING.md**

Cover:

1. **Architecture Overview** — condensed from CLAUDE.md:
   - Directory layout with purpose of each directory
   - C/Rust boundary rules
   - Output routing (serial first, always)
   - Security model (capability-based, no ambient authority)

2. **Coding Standards**:
   - C: K&R braces, `-Wall -Wextra -Werror`, no VLAs, no floating point, no libc
   - Assembly: Intel syntax (NASM), comment every function label
   - Rust: `no_std`, every `unsafe` has `// SAFETY:`, clippy must pass
   - Naming: `subsystem_verb_noun()` in C, `snake_case` in Rust, `SCREAMING_SNAKE` for constants

3. **How to Add a Driver**:
   - Step-by-step walkthrough:
     1. Create `kernel/drivers/mydriver.h` + `kernel/drivers/mydriver.c`
     2. Add to `DRIVER_SRCS` in Makefile
     3. Wire `mydriver_init()` into `kernel/core/main.c`
     4. Print `[MYDRIVER] OK:` or `[MYDRIVER] FAIL:` to serial
     5. If boot.txt changes, update `tests/expected/boot.txt` FIRST (RED)
     6. Write test in `tests/test_mydriver.py`
     7. Run `make test` — must exit 0

4. **Testing Methodology**:
   - TDD: RED before GREEN, always
   - `make test` is the definition of working
   - Python test scripts for interactive tests
   - `make gdb` for debugging panics

5. **Commit Conventions**:
   - Prefix with subsystem: `arch:`, `fs:`, `drivers:`, `tests:`, `docs:`
   - One logical change per commit
   - No `--no-verify`, no force push to main

- [ ] **Step 2: Commit**

```bash
git add CONTRIBUTING.md
git commit -m "docs: add CONTRIBUTING.md with coding standards and driver walkthrough"
```

---

### Task 4: Write docs/architecture.md

**Files:**
- Create: `docs/architecture.md`

- [ ] **Step 1: Write docs/architecture.md**

Full subsystem reference document covering:

1. **Boot Sequence** — GRUB → multiboot2 header → boot.asm (long mode setup) → kernel_main → subsystem init chain

2. **Memory Management**:
   - PMM: bitmap allocator, 4KB pages, `pmm_alloc_page()` / `pmm_free_page()`
   - VMM: 4-level paging (PML4/PDPT/PD/PT), higher-half kernel at 0xFFFFFFFF80000000
   - KVA: bump allocator for kernel virtual addresses, `kva_alloc_pages()` / `kva_free_pages()`
   - Mapped-window: single-slot PTE manipulation for accessing physical pages without identity map

3. **Scheduler**: preemptive round-robin, PIT at 100Hz, circular TCB list, `ctx_switch` in NASM

4. **User Space**: per-process PML4, ELF loader, SYSCALL/SYSRET, TSS for ring transitions

5. **Syscall Layer**: `syscall_entry.asm` → `syscall_dispatch()` → per-syscall handlers; SMAP stac/clac for user memory access

6. **Capability System**: CapSlot Rust type, `cap_grant()` / `cap_check()` FFI, per-process capability table, gated syscalls

7. **VFS**: initrd (static files), console (stdout/stderr), kbd (stdin), pipe, ext2 (persistent storage)

8. **Signals**: `sigaction` / `sigprocmask` / `sigreturn` / `kill`; delivery via iretq and sysret paths

9. **Drivers**:
   - NVMe: PCIe BAR0 MMIO, admin/IO queue pairs, doorbell+poll, blkdev registration
   - xHCI: USB host controller, HID boot protocol, event ring polling at 100Hz
   - AMD DC: DCN 2.1/3.0/3.1, EDID via DDC I2C, HUBP/DPP/OPP/OPTC/DIO pipeline

10. **Framebuffer**: abstraction layer, VESA backend (multiboot2 tag 8), AMD DC backend, 8x16 font, scrolling console

11. **Design Decisions**:
    - Why capability-based (not DAC/MAC)
    - Why GRUB + ISO (not direct `-kernel` boot)
    - Why serial-first output (debugging is paramount)
    - Why polling (simplicity over MSI complexity in v1.0)
    - Why no SMP (correct single-core first, scale later)

- [ ] **Step 2: Commit**

```bash
git add docs/architecture.md
git commit -m "docs: add architecture.md full subsystem reference"
```

---

### Task 5: Write docs/hardware-support.md

**Files:**
- Create: `docs/hardware-support.md`

- [ ] **Step 1: Write docs/hardware-support.md**

Cover:

1. **Tested Hardware** — table format:
   | Component | Model | Status | Notes |
   |-----------|-------|--------|-------|
   | CPU | AMD Ryzen 5 5600G | Tested | Zen 3, SMAP supported |
   | CPU | AMD Ryzen 7 7700X | Untested | Zen 4, expected compatible |
   | iGPU | AMD Cezanne (DCN 2.1) | Tested | Native resolution via AMD DC |
   | iGPU | AMD Rembrandt (DCN 3.0) | Untested | Register tables included |
   | iGPU | AMD Phoenix (DCN 3.1) | Untested | Register tables included |
   | Storage | Any NVMe M.2 | Tested | NSID=1, 512-byte sectors |
   | Keyboard | USB HID (boot protocol) | Tested | Via xHCI root hub |
   | Keyboard | PS/2 | Tested | Standard scancode set 1 |

2. **QEMU Compatibility**:
   - `-machine pc` (SeaBIOS): full support via ISO boot
   - `-machine q35` (SeaBIOS): PCIe, NVMe, xHCI
   - `-machine q35` + OVMF: full UEFI disk boot

3. **Supported Features**:
   - NVMe read/write (polling, no MSI)
   - ext2 filesystem (read-write, no journal)
   - USB HID keyboard (boot protocol, polling at 100Hz)
   - AMD DC display (DCN 2.1/3.0/3.1, single output)
   - VESA framebuffer (any GRUB-supported GPU)
   - Interactive shell with pipes, signals, file utilities

4. **Known Limitations**:
   - Single-core only (no SMP)
   - No networking
   - No USB mass storage
   - No USB hubs
   - No AHCI/SATA
   - No audio
   - No power management (ACPI S-states)
   - No Secure Boot
   - 128KB max file size (ext2 single-indirect only)
   - No journal (ext2, not ext3/4)

5. **Adding Hardware Support** — pointer to CONTRIBUTING.md

- [ ] **Step 2: Commit**

```bash
git add docs/hardware-support.md
git commit -m "docs: add hardware-support.md with tested hardware and known limitations"
```

---

### Task 6: Update CLAUDE.md final build status

**Files:**
- Modify: `.claude/CLAUDE.md`

- [ ] **Step 1: Update the Build Status table**

Mark all phases through 26 as done:

```markdown
| PCIe enumeration + ACPI (Phase 19) | ✅ Done | MCFG+MADT on q35; graceful skip on -machine pc |
| NVMe driver + blkdev (Phase 20) | ✅ Done | Admin+IO queues; blkdev registration; doorbell+poll I/O |
| ext2 read-write filesystem (Phase 21) | ✅ Done | Block cache; path walk; create/write/unlink/mkdir |
| xHCI + USB HID keyboard (Phase 22) | ✅ Done | Device enumeration; HID boot protocol; kbd_usb_inject |
| Framebuffer + VESA backend (Phase 23) | ✅ Done | Multiboot2 FB tag; 8x16 font; printk FB path |
| AMD DC framebuffer (Phase 24) | ✅ Done | DCN 2.1/3.0/3.1; EDID; VRAM scanout; real-hardware only |
| Installable disk image (Phase 25) | ✅ Done | GPT parse; mkdisk.sh; UEFI boot via OVMF; make install |
| Documentation + v1.0 release (Phase 26) | ✅ Done | README, CONTRIBUTING, architecture, hardware-support, LICENSE |
```

- [ ] **Step 2: Add final timestamp**

```markdown
*Last updated: 2026-03-XX — Phase 26 complete. Aegis v1.0.0 released. All subsystems GREEN.*
```

- [ ] **Step 3: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs: finalize CLAUDE.md build status for v1.0.0 release"
```

---

### Task 7: Tag v1.0.0 release

**Files:** None (git operation only)

- [ ] **Step 1: Run full test suite**

```bash
make test 2>&1 | tail -10
```

All tests must pass. Do NOT tag if any test fails.

- [ ] **Step 2: Run extended test suite**

```bash
./tests/run_tests.sh
```

All Python smoke tests must pass.

- [ ] **Step 3: Verify clean working tree**

```bash
git status
```

No uncommitted changes.

- [ ] **Step 4: Create signed tag**

```bash
git tag -s v1.0.0 -m "Aegis v1.0.0 — first public release

Capability-based POSIX-compatible x86-64 kernel.

Features:
- Boots from NVMe on real AMD Ryzen hardware via UEFI/GRUB
- ext2 read-write filesystem with persistent storage
- Interactive shell with pipes, signals, I/O redirection
- USB HID keyboard via xHCI
- AMD DC native display output (DCN 2.1/3.0/3.1)
- VESA framebuffer fallback for QEMU and other GPUs
- Capability-gated syscalls (no ambient authority)
- 15+ user-space utilities (sh, ls, cat, echo, wc, grep, sort, ...)

Tested on: QEMU (pc/q35/OVMF), AMD Ryzen 5000G+
License: MIT"
```

If GPG signing is not configured, use an annotated tag instead:

```bash
git tag -a v1.0.0 -m "Aegis v1.0.0 — first public release"
```

- [ ] **Step 5: Verify tag**

```bash
git tag -l v1.0.0
git show v1.0.0 --quiet
```

- [ ] **Step 6: Push tag (when ready to publish)**

```bash
git push origin v1.0.0
```

Only do this when the user explicitly asks to publish.

---

## Final Verification

```bash
make test
```

Expected: exit 0.

```bash
./tests/run_tests.sh
```

Expected: all tests pass.

```bash
git log --oneline -10
```

Expected: documentation commits followed by the v1.0.0 tag.

```bash
ls LICENSE README.md CONTRIBUTING.md docs/architecture.md docs/hardware-support.md
```

Expected: all five files exist.
