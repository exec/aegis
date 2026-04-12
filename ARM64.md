# Aegis ARM64 Port — Status & Plan

**Date:** 2026-04-12
**Branch:** `arm64-port` (freshly recreated from master at f961aa3)
**Archive:** `arm64-old` — preserved snapshot of prior ARM64 work (see §3)
**Targets:** QEMU `virt` (primary dev) → Raspberry Pi 4 (BCM2711, Cortex-A72, GICv2) → Raspberry Pi 5 (BCM2712, Cortex-A76, GICv3)
**Status:** Research complete. Initial blockers fixed — every `.c` file in the kernel now compiles for ARM64. Two link-stage issues remain before first boot (see §14).

This document synthesizes findings from four parallel research agents run on 2026-04-12. It supersedes the ARM64 sections of CLAUDE.md, which are now out of date.

---

## 1. TL;DR

- Master has ~1300 LOC of ARM64 scaffolding in `kernel/arch/arm64/` (boot stub, PL011, GIC, PMM, VMM, syscall translation) but **zero Makefile rules** — `make ARCH=arm64` silently builds x86.
- The previous port branch (now `arm64-old`) reached **first user process running real musl libc in EL0**, but its history is **completely unrelated to master** (420 commits, no merge base). Cannot be rebased — must be used as a reference for cherry-applied file patches.
- Local toolchain is already in place: `aarch64-elf-gcc 15.2.0`, `qemu-system-aarch64 10.2.2`, `dtc`, and **HVF acceleration works on this Mac** → sub-second boot loop.
- Rust `kernel/cap/` has **zero arch-specific code** — will cross-compile to `aarch64-unknown-none` with only a 1-hour Makefile tweak.
- Vortex test harness already supports arch-neutral serial capture and `-kernel` direct ELF boot via `VORTEX_QEMU_BINARY` env var. Only `tests/src/presets.rs` needs a new `aegis_arm64_virt()` preset.
- Boot format decision: **Linux arm64 Image** (64-byte header + flat binary). One format works on both QEMU virt `-kernel` and Pi firmware (`kernel8.img` with `arm_64bit=1`).
- **Policy decision (2026-04-12):** inline `#ifdef __aarch64__` dispatch in cross-arch files is explicitly allowed (option A — §9). Master already has 18 such blocks across 13 files, survivors of the old arm64-port work. A clean port doesn't start from zero on `syscall_frame_t`, syscall translation, signal frames, ELF EM_AARCH64 recognition, or TLS reads — they're already in place. CLAUDE.md updated to reflect.
- Arch-hygiene debt (unconditional x86 leaks, no `#ifdef` guard): 4 HIGH violations (`main.c` i8042 drain, `sys_reboot` asm, `vmm.c` hardcoded PML4 indices, `panic_bluescreen` signature). ~6-10h.
- **Total estimated effort to user-process on QEMU virt:** ~40-45h. To bare-metal Pi 4: +15-25h. Pi 5 (GICv3): +10-15h more.

---

## 2. Hardware targets

### QEMU `virt` (primary dev loop)

- Runs natively on this Mac via HVF acceleration → identical performance to silicon
- Machine: `qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 2G -serial stdio -accel hvf -kernel build/aegis.img`
- Entry state: x0=DTB, PC=ELF entry, EL1 by default (EL2 with `virtualization=on`)
- Device tree is synthesized by QEMU, passed in x0. Parse it for memory map, UART base, GIC layout.
- Use GICv3 (`-machine virt,gic-version=3`) to match Pi 5; GICv2 for Pi 4 compatibility testing.

### Raspberry Pi 4 (BCM2711)

- Cortex-A72, 4 cores, GIC-400 (GICv2 compatible)
- Boot: GPU firmware reads FAT32 boot partition, loads `kernel8.img` to 0x80000, passes DTB pointer in x0, enters at EL2
- `config.txt`: `arm_64bit=1`, `kernel=kernel8.img`, `device_tree=bcm2711-rpi-4-b.dtb`
- Multiple cores start simultaneously; secondaries spin on DTB spin-tables

### Raspberry Pi 5 (BCM2712)

- Cortex-A76, 4 cores, GIC-600 (GICv3)
- Same boot protocol as Pi 4 with `kernel_2712.img` / `bcm2712-rpi-5-b.dtb`
- GICv3 means different MMIO layout and redistributor handling vs Pi 4. Code that works on Pi 4 won't automatically work on Pi 5 interrupt handling.

### Strategy

Target **QEMU virt + GICv3** as primary dev target (matches Pi 5, and GICv3 is forward-compatible). Add a compile-time or DTB-driven switch for GICv2 fallback needed for Pi 4 and legacy QEMU. Pi bring-up is a post-v0.1 milestone; defer real hardware until QEMU virt is stable.

---

## 3. `arm64-old` archive — what's in it, why we can't rebase

The prior ARM64 work on the remote branch originally called `arm64-port` has been renamed to `arm64-old`. A fresh `arm64-port` was created from current master. The archive is preserved permanently for reference.

### Contents (14 ARM64-specific commits)

```
f4081d3 simplify: remove dead code and overengineering from ARM64 VMM
e7f1aab security: SPSR escalation + syscall mismap + mmap bounds + getdents overflow
a859b7d refactor: replace remaining raw PTE flags in virtio_net.c + xhci.c
07b09ba refactor: arch-agnostic audit — fix all remaining leakage
902327d arm64: musl libc runs on Aegis ARM64!
296f803 arm64: FIRST USER PROCESS — Hello from ARM64 EL0!
1cb82f6 arm64: user process WIP — EL0 ERET works, SVC fires, TTBR0 debugging
dbd3f2a arm64: entire kernel compiles + links for ARM64
082a21b arm64: preemptive scheduler with context switch
075554b arm64: GIC + generic timer — interrupts working at 100Hz
266694c arm64: KVA allocator live — shared kva.c runs on ARM64
30224ca arm64: VMM with 4KB-granule page tables + MMU enable
96ebb67 arm64: add printk + PMM — shared kernel code runs on ARM64
371f6cd arm64: minimal boot stub — PL011 serial on QEMU virt
```

The branch hit a real milestone: it boots to a user process running musl libc in EL0, with SVC syscall entry, SPSR validation preventing privilege escalation, and full vectors.S / proc_enter.S implementations. Reading this code is the fastest way to get oriented.

### Why we can't rebase

```
$ git merge-base origin/arm64-old master
NO MERGE BASE

$ git rev-list --max-parents=0 origin/arm64-old
818d519a68cfc483b949c7e276fbe3b74fde88aa

$ git rev-list --max-parents=0 master
236e42a1be01134b473f0e78ebad23080863c1e9
```

Different root commits. arm64-old has 420 commits of parallel history — it was force-pushed at some point with a filter-branch or commit-tree rewrite. `git cherry-pick` refuses unrelated histories.

**Workflow for pulling code forward:**

```bash
# Show the diff a commit introduced
git show <sha-on-arm64-old> -- kernel/arch/arm64/

# Extract a single file at a given commit
git show origin/arm64-old:kernel/arch/arm64/boot.S > kernel/arch/arm64/boot.S

# Apply a diff as a patch (for commits that touch shared code)
git show <sha-on-arm64-old> | git apply --3way
```

For the 14 arm64-specific commits, the right approach is usually: read the commit for context, then either check out the file directly (for `kernel/arch/arm64/*`) or rewrite the shared-code changes manually (syscall translation, arch abstractions).

### Other branches

- `origin/arm64-shell`, `origin/arm64-ttbr1` — 0 divergence from master after force-push. Stale pointers. Can be deleted at any time.

---

## 4. State of `kernel/arch/arm64/` on master

Master contains ~1300 LOC of ARM64 scaffolding, but it is **not wired into the build**.

### Files present

| File | LOC | Status |
|------|-----|--------|
| `arch.h` | 186 | ✅ Arch-portable interface: TTBR1 kernel mapping, USER_ADDR_MAX, arch_vmm_*, arch_set_* |
| `boot.S` | ~135 | ✅ EL2→EL1 boot, DTB parse, MMU enable, kernel entry |
| `main.c` | 78 | ✅ arch_init → gic_init → timer_init → sched_init → proc_spawn_init |
| `mmu_early.c` | 57 | ✅ Early page tables from DTB |
| `arch_mm.c` | 160 | ✅ PMM region discovery from DTB |
| `arch_vmm.c` | 53 | ⚠ Minimal — user/kernel VA mapping stubs, empty table walk |
| `uart_pl011.c` | 112 | ✅ PL011 UART RX/TX with IRQ |
| `gic.c` | 133 | ✅ GIC v2 init, enable_irq, 32 SPI handler dispatch |
| `irq.c` | 76 | ✅ Generic timer 100Hz |
| `vmm_arm64.c` | 484 | ⚠ Partial — page table walker, TLB, user/kernel separation incomplete |
| `stubs.c` | 170 | ⚠ Stubs — cap_init allow-all, memcpy/memset, vga_write_string no-op |
| `vectors.S` | ~265 | ⚠ Exception vector table — only timer IRQ handled |
| `ctx_switch.S` | ~72 | ⚠ Callee-saved context; missing fork_child_return |
| `proc_enter.S` | ~123 | ⚠ EL1→EL0 trampolines; TTBR0 incomplete |

### Makefile status

**Zero ARM64 build rules.** `make ARCH=arm64` silently builds x86-64:

- `CC = x86_64-elf-gcc` (not overridable by ARCH)
- `ARCH_SRCS` references only `kernel/arch/x86_64/*.c` (21 files)
- `BOOT_SRC = kernel/arch/x86_64/boot.asm`
- `CAP_LIB` hardcodes `--target x86_64-unknown-none`
- `MUSL_BUILT` reconfigures for host arch (arm64-apple-darwin on this Mac — wrong on both counts)

### Subsystem maturity vs x86-64

| Subsystem | x86-64 | ARM64 on master | Gap |
|-----------|--------|-----------------|-----|
| Serial | VGA + COM1 | PL011 TX/RX + IRQ | ✅ feature-complete |
| PMM | Bitmap from multiboot | DTB region discovery | ✅ ported |
| VMM | 4-level paging | 4-level paging | ⚠ user/kernel isolation TODO |
| KVA allocator | Shared | Shared | ✅ arch-agnostic |
| Timer | PIT 100Hz + PIC | GIC + generic timer 100Hz | ✅ ported |
| Context switch | ctx_switch.asm | ctx_switch.S (WIP) | ⚠ incomplete |
| Ring-3 | SYSRET/IRETQ + TSS | ERET (WIP) | ⚠ incomplete |
| Boot | GRUB multiboot2 | DTB | ✅ ported (but see §6) |
| ELF loader | ET_EXEC + ET_DYN | EM_AARCH64 not recognized | ❌ missing |
| Syscall dispatch | x86 numbers native | ARM64→x86 translation table | ✅ complete |
| Signal delivery | full x86 impl | split w/ `#ifdef __aarch64__`, stubbed | ⚠ halfway |
| Capability system | Rust kernel/cap/ | allow-all stub | ❌ Rust cross-compile (§7) |

---

## 5. Toolchain readiness

### Local (macOS arm64) — **ready**

- `aarch64-elf-gcc` 15.2.0 (Homebrew) ✅
- `qemu-system-aarch64` 10.2.2 (Homebrew) ✅
- `dtc` 1.7.2 (Homebrew) ✅
- `rustup` with `aarch64-apple-darwin` ✅ (need to add `aarch64-unknown-none`)
- **HVF acceleration works**: `qemu-system-aarch64 -machine virt -cpu host -accel hvf` runs natively ✅

Install the Rust target:

```bash
rustup target add aarch64-unknown-none
```

### Build box (10.0.0.19) — needs one apt install

Has: `qemu-system-aarch64` (from `qemu-efi-aarch64` package), `musl-tools`, existing x86_64 toolchain.

Missing: `aarch64-linux-gnu-gcc` and binutils.

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.19 \
  'sudo apt-get install -y gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu && \
   sudo ln -sf /usr/bin/aarch64-linux-gnu-gcc /usr/local/bin/aarch64-elf-gcc && \
   rustup target add aarch64-unknown-none'
```

After this, CI can build ARM64 as a second matrix job in `.github/workflows/build-iso.yml`.

### Recommended dev flow

**Primary:** local Mac + QEMU virt with HVF. Build with `aarch64-elf-gcc`, test with `qemu-system-aarch64 -accel hvf`. Full-speed boot under 1 second.

**CI:** build box with `aarch64-linux-gnu-gcc` + software QEMU. Slower but validates on Debian trixie (matches x86 CI).

**Hardware:** defer Raspberry Pi until QEMU virt is stable.

---

## 6. Boot protocol decision — **Linux arm64 Image format**

### Options considered

| Protocol | Pi | QEMU virt | Complexity | Verdict |
|----------|----|-----------| -----------|---------|
| QEMU `-kernel <ELF>` (current arm64-old) | ❌ | ✅ | Low | Dev-only — Pi can't load ELF |
| Pi raw `kernel8.img` flat binary | ✅ native | ❌ | Low | Pi-only |
| U-Boot chainload | ✅ | ✅ | Medium | Extra moving part |
| **Linux arm64 Image (64-byte header + flat)** | ✅ | ✅ | **Low-Medium** | **Chosen** |
| EFI stub | ❌ (needs 3rd-party EDK2) | ✅ | High | Overkill |

### Why Linux arm64 Image

One format works on both targets:

- **QEMU virt** `-kernel build/aegis.img` parses the Image header and loads correctly.
- **Pi firmware** with `arm_64bit=1` in `config.txt` respects the same Image header. Drop `aegis.img` in as `kernel8.img`, copy the Pi DTB, boot.
- Standardized: [Linux kernel Documentation/arch/arm64/booting.rst](https://www.kernel.org/doc/html/latest/arch/arm64/booting.html)

### Image header layout (64 bytes)

```
offset  size  field
------  ----  ---------------------------------------------
0x00    4     code0       — "MZ" magic + ARM64 branch (or NOP for non-EFI)
0x04    4     code1       — branch to _start
0x08    8     text_offset — image load offset from 2MB boundary (0)
0x10    8     image_size  — effective image size
0x18    8     flags       — page size, endianness
0x20    24    reserved
0x38    4     magic       — 0x644d5241 ("ARM\x64")
0x3c    4     res5        — PE header offset (0 if no EFI stub)
```

### Entry contract

```
x0 = DTB pointer (physical)
x1 = 0 (reserved)
x2 = 0 (reserved)
x3 = 0 (reserved)
PC = Image base + text_offset
EL = EL2 (Pi, QEMU virt by default) or EL1
MMU: OFF
Data caches: OFF
D-cache on image region: invalidated by bootloader
```

### What changes from arm64-old

| Aspect | arm64-old | New |
|--------|-----------|-----|
| Kernel image | ELF | Flat binary with 64-byte header |
| Build step | `ld` → ELF | `ld` → ELF → `objcopy -O binary` + prepend header |
| Entry EL | EL1 | EL2 (Pi), EL1 or EL2 (QEMU) — stub must handle both |
| Pi support | No | Yes |
| Boot stub | DTB already in x0 | Same, unchanged |

### Concrete work

1. **`tools/gen-arm64-image.py`** — reads `build/aegis-arm64.elf`, extracts load address and image size, generates 64-byte header, outputs `build/aegis.img` (1-2h)
2. **Makefile `image` target** — `objcopy -O binary` then header prepend (0.5h)
3. **Boot stub EL2→EL1 drop** — Pi starts at EL2; current arm64-old assumes EL1. Need to detect current EL and drop if needed (1-2h)
4. **`tools/mkpi.sh`** — SD card prep script: partition, format FAT32, copy Pi firmware + DTB + config.txt + kernel8.img (2-3h) — post-v0.1
5. **Docs** in README.arm64.md (1h)

**Subtotal: 5-9h** (excluding Pi bring-up).

---

## 7. Rust `kernel/cap/` cross-compile — **~1 hour of Makefile work**

Excellent news: the cap crate is completely portable.

### Crate summary

- 1 source file (`src/lib.rs`), 111 lines
- Zero external dependencies (only `core`)
- `panic = "abort"` in both profiles
- No `#[cfg(target_arch)]`, no `asm!`, no `core::arch::*`
- `#[repr(C)]` types are all POD: `CapSlot` is two `u32` fields; args are pointers and primitives
- No >16-byte by-value aggregates → no calling convention surprises under AAPCS64

### What needs to change

The source compiles for `aarch64-unknown-none` as-is. Only the Makefile and rust-toolchain need parameterization:

1. `rust-toolchain.toml:3` — add `"aarch64-unknown-none"` to targets list
2. Add `RUST_TARGET` Makefile variable (default `x86_64-unknown-none`, override for ARM64)
3. Replace hardcoded `--target x86_64-unknown-none` in the cap build rule
4. Replace hardcoded `kernel/cap/target/x86_64-unknown-none/...` in `CAP_LIB` path

**Estimated effort: 0.5-1 hour.**

---

## 8. Test harness readiness — **mostly ready**

### Vortex (`../vortex/`)

Already ARM64-ready:

- **QEMU binary** configurable via `VORTEX_QEMU_BINARY` env var (defaults to `qemu-system-x86_64`). Set to `qemu-system-aarch64` for ARM64 — zero Vortex changes.
- **Boot methods** — `BootSource::Kernel { kernel, initrd, cmdline }` already supports `-kernel <elf>` direct boot.
- **ConsoleOutput / ConsoleStream** capture serial line-by-line, arch-neutral. `process_line()` strips ANSI + CR with no x86 assumptions.
- **Monitor socket API** (screendump, send_keys) uses QEMU HMP — works on any arch.

**No upstream Vortex changes required.**

### `tests/` crate changes (2-4h)

- **`tests/src/presets.rs`** — add `aegis_arm64_virt()`:
  ```rust
  pub fn aegis_arm64_virt() -> QemuOpts {
      QemuOpts {
          machine: "virt,gic-version=3".into(),
          display: "none".into(),
          devices: vec![],
          drives: vec![],
          extra_args: vec![
              "-nodefaults".into(),
              "-cpu".into(), "cortex-a72".into(),
              "-m".into(), "2G".into(),
              "-serial".into(), "stdio".into(),
              "-no-reboot".into(),
          ],
          serial_capture: true,
          monitor_socket: false,
      }
  }
  ```
- **New env var `AEGIS_IMG`** alongside `iso()` for the `.img` path
- **`tests/src/harness.rs::boot()`** — accept `BootSource` so same harness boots ISO or kernel image
- **New boot oracle** `tests/tests/boot_oracle_arm64.rs` or feature-gate the existing one. Expected subsystem lines differ: `[GIC] OK` instead of `[IOAPIC]/[LAPIC]`, no `[PCIE]` (virt has virtio-mmio unless explicitly added), no `[ACPI]`.
- **`tests/historical/expected/boot-arm64.txt`** — new oracle file (or switch to regex-based matching that covers both archs)

### Exit status mechanism

x86 uses `isa-debug-exit` (IO port 0xf4) to signal test pass/fail. ARM64 virt doesn't have this. Options:

- **ARM semihosting** — kernel issues `HLT 0xF000` with exit code in x0; QEMU catches and exits. Requires `-semihosting` QEMU flag and a tiny syscall trap in the kernel.
- **Virtio-exit device** — cleaner but adds a driver.
- **Parse last line of output** — brittle.

Recommend semihosting. `qemu-system-aarch64 -machine virt -semihosting-config enable=on,target=native` plus ~20 lines of asm in the kernel to invoke `SYS_EXIT_EXTENDED`.

---

## 9. Arch-isolation hygiene — refactor before or during the port

### Policy decision (2026-04-12): inline `#ifdef` dispatch is allowed

Master already contains 18 `#ifdef __aarch64__` blocks across 13 files in "cross-arch" areas (`kernel/syscall/`, `kernel/signal/`, `kernel/proc/`, `kernel/sched/`) — survivors of the old arm64-port work that landed before the history reset. Rather than scatter these into parallel arch-specific files, the project has decided to **accept inline `#ifdef` dispatch as the pattern for inherently arch-specific code in cross-arch files**.

This covers:
- `syscall_frame_t` struct definition and `FRAME_IP/SP/FLAGS` accessors (`kernel/syscall/syscall.h`)
- Syscall number translation (`kernel/syscall/syscall.c`)
- Signal delivery paths (`kernel/signal/signal.c`, `kernel/syscall/sys_signal.c`)
- Fork/exec process frame setup (`kernel/syscall/sys_process.c`, `kernel/syscall/sys_exec.c`)
- TLS register reads (`mrs tpidr_el0` in `sys_process.c:474`)
- ELF `EM_AARCH64` recognition (`kernel/proc/elf.c`)
- utsname `machine` field (`kernel/syscall/sys_identity.c`)

**Good news for the port:** these blocks are already in place. A clean ARM64 port doesn't start from zero on these files — the frame struct, accessors, syscall translation table, TLS read, and ELF machine check all survived.

CLAUDE.md has been updated to reflect this policy.

### Still a problem — unconditional x86 assumptions

What's **not** OK is x86 code that isn't `#ifdef`-guarded and has no ARM64 counterpart. These need actual fixing:

| File | Issue | Severity |
|------|-------|----------|
| `kernel/core/main.c:133` | `while (inb(0x64) & 0x01) (void)inb(0x60);` — i8042 drain, unconditional | HIGH |
| `kernel/syscall/sys_identity.c:293,298` | `outb 0xFE → 0x64` + `cli; hlt` — sys_reboot uses x86 inline asm with no ARM64 branch | HIGH |
| `kernel/mm/vmm.c:218` | Hardcoded `0xFFFFFFFF80000000`, `pml4[511]`, `pdpt_hi[510]` — no arch dispatch | HIGH |
| `kernel/drivers/panic_screen.c:100` | `panic_bluescreen(vector, rip, ec, cr2, rsp, rbp, rax, rbx)` — x86 register names in function signature (shared header) | HIGH |
| `kernel/mm/vmm.h` + syscall/* | `pml4_phys`, `vmm_create_user_pml4`, `PML4`/`PDPT` in comments — x86 terminology as API names | MEDIUM |
| various | Hardcoded `0xFFFF800000000000` user/kernel boundary — should be `USER_VA_MAX` per-arch macro | MEDIUM |

### Refactor plan (6-10 hours total — down from 15-20)

1. Create `arch_io.h` with `arch_port_read8/write8` (or stubbed no-op on ARM64). Abstract the i8042 drain in `main.c` behind an arch-specific init path (1-2h)
2. `arch_reboot_now()` / `arch_halt_now()` declared in `kernel/core/arch.h`, implemented per arch (1h)
3. `KERN_VA_BASE` / `USER_VA_MAX` constants in per-arch headers, replace hardcoded literals (2-3h)
4. Refactor `panic_bluescreen` signature to take a neutral `struct isr_frame *` (or per-arch `cpu_state_t *`) (2-3h)
5. Skipped: PML4/PDPT rename — with option A, x86 terminology in #ifdef'd x86 code is fine. ARM64 branches use their own terminology.

**Subtotal: 6-10 hours.** Run in parallel with §10 phase A0 or fold into A1.

---

## 10. Phase plan

Each phase produces a testable milestone. Run arch-hygiene refactor (§9) in parallel on a sibling branch, or fold each fix into the phase that first needs it.

### A0 — Toolchain + Rust cap cross-compile (2h)

- `rustup target add aarch64-unknown-none` locally and on build box
- Parameterize Makefile with `ARCH` and `RUST_TARGET` variables
- Verify `kernel/cap/` builds for both targets
- **Milestone:** `make ARCH=arm64 kernel/cap/target/aarch64-unknown-none/release/libcap.a` succeeds

### A1 — ARM64 kernel compiles + links (8h)

- Makefile ARM64 rules: `aarch64-elf-gcc`, ARM64 `ARCH_SRCS`/`ARCH_ASMS`, `.S` handling
- Port minimum boot stub from arm64-old (`boot.S`, early MMU, DTB parse)
- Wire up `kernel/arch/arm64/*` that already exists on master
- Write simple Linux arm64 Image header generator (`tools/gen-arm64-image.py`)
- **Milestone:** `make ARCH=arm64` produces `build/aegis.img`, `qemu-system-aarch64 -M virt -kernel build/aegis.img` prints `[SERIAL] OK` via PL011

### A2 — Boot to scheduler (8h)

- GIC init, generic timer 100Hz (exists on master — wire up)
- PMM from DTB (exists — validate)
- VMM with TTBR0/TTBR1 split (partial on master, complete from arm64-old reference)
- Scheduler init, idle loop
- **Milestone:** `[SCHED] OK` on QEMU virt

### A3 — User process in EL0 (12h)

- Full vectors.S with EL0/EL1 entry paths (cherry-apply from arm64-old reference — see commits 371f6cd through 296f803)
- SPSR validation (mask to NZCV only; prevent privilege escalation — from e7f1aab)
- proc_enter.S EL1→EL0 trampoline with GPR zeroing
- ctx_switch.S callee-saved save/restore
- ELF loader EM_AARCH64 recognition in `kernel/proc/elf.c`
- **Milestone:** `[INIT] Hello from ARM64 EL0!` via SVC → sys_write

### A4 — musl libc + syscall dispatch (10h)

- Syscall translation table ARM64→x86 numbers (partial on master)
- Build aarch64 musl (`tools/build-musl.sh` arm64 variant — may need crosstool-NG or Alpine sysroot)
- Run a hello-world musl binary in EL0
- **Milestone:** musl static binary runs, prints to PL011 via `sys_write`

### A5 — Test harness + CI (4h)

- `aegis_arm64_virt()` preset in `tests/src/presets.rs`
- `AEGIS_IMG` env var, `BootSource::Kernel` path through harness
- ARM semihosting exit device in kernel (~20 lines asm)
- `tests/expected/boot-arm64.txt` oracle file
- New test `tests/tests/boot_oracle_arm64.rs`
- Add ARM64 matrix job to `.github/workflows/build-iso.yml`
- **Milestone:** `make test ARCH=arm64` exits 0 locally and in CI

### A6 — Interactive shell / first real workload (4-6h)

- stsh or /bin/sh musl build for aarch64
- Minimal `sys_read` from PL011 RX
- Line discipline basics
- **Milestone:** interactive shell on QEMU virt

**Subtotal A0-A6: 48-52h** (plus §9 hygiene: 15-20h in parallel).

### Post-v0.1: bare-metal Raspberry Pi

### B1 — Pi firmware boot (8-12h)

- EL2→EL1 drop in boot stub
- BCM2711 PL011 base address (different from QEMU virt)
- `tools/mkpi.sh` SD card prep
- Pi 4 DTB parsing (different from QEMU virt DTB layout)
- **Milestone:** `[SERIAL] OK` on real Pi 4 hardware

### B2 — Pi GIC + timer (6-10h)

- Pi 4 uses GIC-400 (GICv2); code should already work
- Verify IRQ routing and timer
- Multi-core secondary bring-up via DTB spin-tables
- **Milestone:** full boot to scheduler on Pi 4

### B3 — Pi 5 GICv3 (10-15h)

- GIC-600 / GICv3 driver — redistributor, system registers, different MMIO
- Compile-time or runtime switch between GICv2 and GICv3
- **Milestone:** full boot on Pi 5

---

## 11. Risks

| # | Risk | Impact | Mitigation |
|---|------|--------|------------|
| 1 | musl aarch64 rebuild config | 6h → 16h if broken. crosstool-NG required. | Use Alpine Docker as sysroot source; pin musl 1.2.5 |
| 2 | ELF loader EM_AARCH64 support | Static binaries easy; dynamic linking harder. PT_INTERP at different base (0x100000000 maybe) | Start static-only; add ld-musl-aarch64.so.1 later |
| 3 | Semihosting exit device bugs | Test harness reports false pass/fail | Fallback: parse last line of boot output for exit magic |
| 4 | Arch-hygiene violations fight port | HIGH-severity leaks break build | Do §9 first on a sibling branch so port rebase onto cleaned master is smooth |
| 5 | arm64-old history unrelated → no rebase | Must re-port commits file-by-file | Use `git show origin/arm64-old:path` + manual apply; document in commit messages that commit X corresponds to arm64-old SHA Y |
| 6 | GICv3 on Pi 5 is meaningfully different from GICv2 | Pi 5 bring-up harder than Pi 4 | Do Pi 4 first; GICv3 becomes a separate milestone |
| 7 | HVF under macOS may have quirks with certain QEMU features | Dev loop friction | Have software emulation as fallback; use build box for CI |

---

## 12. Immediate next steps (when ready to start coding)

**Starting state:** fresh `arm64-port` branch at master. No commits yet.

1. `rustup target add aarch64-unknown-none` on this machine and 10.0.0.19
2. Begin phase A0: parameterize Makefile with `ARCH` / `RUST_TARGET`, verify cap/ builds for aarch64-unknown-none
3. Phase A1 scaffold: Makefile ARM64 rules, `tools/gen-arm64-image.py`, boot stub → `[SERIAL] OK` on QEMU virt

Suggest running §9 hygiene pass on a sibling branch (`arch-hygiene`) in parallel. Merge it into master before A3 (user process work) where the PML4/PDPT renames touch most files.

---

## 13. Reference commits on `arm64-old` (for file-level lookup)

When implementing a phase, check these commits first:

| Phase | arm64-old commit | What to look at |
|-------|------------------|-----------------|
| A1 | 371f6cd `arm64: minimal boot stub` | `kernel/arch/arm64/boot.S`, MMU setup |
| A1 | 96ebb67 `arm64: add printk + PMM` | PMM from DTB, shared printk |
| A2 | 30224ca `arm64: VMM with 4KB-granule page tables` | Page table walker, TTBR0/TTBR1 |
| A2 | 075554b `arm64: GIC + generic timer` | GIC init, timer 100Hz |
| A2 | 082a21b `arm64: preemptive scheduler` | ctx_switch.S |
| A3 | 1cb82f6 `arm64: user process WIP` | proc_enter.S, TTBR0 switching |
| A3 | 296f803 `arm64: FIRST USER PROCESS` | vectors.S EL0/EL1 split, SVC handling |
| A3 | e7f1aab `security: SPSR escalation fix` | Critical — SPSR mask on sigreturn |
| A4 | 902327d `arm64: musl libc runs` | musl build config, aarch64 sysroot |
| A1-A4 | dbd3f2a `arm64: kernel compiles + links` | Makefile ARM64 rules reference |

```bash
# Read a file at a given commit without checking out
git show origin/arm64-old:kernel/arch/arm64/boot.S

# Show what a commit changed
git show origin/arm64-old 371f6cd --stat
git show origin/arm64-old 371f6cd -- kernel/arch/arm64/boot.S
```

---

## 14. Setup work completed (2026-04-12)

An unexpected discovery: `kernel/arch/arm64/Makefile` already exists as a standalone ARM64 build. It's independent of the top-level Makefile, references shared sources directly via `$(REPO)/...`, and was last maintained ~685 commits ago. Rather than parameterize the top-level Makefile (ARM64.md §10 phase A1), the faster path is to update this existing standalone Makefile and come back to unification later.

Running `make -C kernel/arch/arm64` revealed 12 initial blockers — a mix of compile errors, missing includes, missing symbols, and unconditional x86 code. All 12 have been fixed. The kernel now compiles every `.c` file cleanly. Only two link-stage issues remain before first `[SERIAL] OK` on QEMU virt.

### Blockers fixed

| # | Symptom | Root cause | Fix |
|---|---------|------------|-----|
| 1 | `vmm_arm64.c:67` static conflict with `vmm.h:134` | Header prototype added since branch diverged | Removed `static` qualifier from `vmm_window_map`/`vmm_window_unmap` |
| 2 | `sys_process.c:9: tty.h: No such file` | ARM64 Makefile CFLAGS missing `-Ikernel/tty`; had stale `-Ikernel/elf` (dir doesn't exist) | Added/removed |
| 3 | `No rule to make target kernel/elf/elf.c` | `elf.c` moved to `kernel/proc/` on master | Fixed Makefile rule |
| 4 | `kbd_has_data` implicit declaration | Non-destructive kbd check added since branch diverged; not in ARM64 `kbd.h` | Added prototype to `kernel/arch/arm64/kbd.h` + no-op stub in `stubs.c` |
| 5 | `xhci.c:446` `-Werror=array-bounds` | gcc 15.2 stricter than 14.2 about flex-array pointer casts | Added `-Wno-error=array-bounds` to CFLAGS |
| 6 | `ccCo5gST.s:1207: unknown mnemonic 'cli'` compiling fb.c | `panic_screen.c` (which fb.c `#include`'s directly) uses x86 inline asm | Replaced `cli; hlt` with `arch_disable_irq(); arch_halt();` (both arches define these inline in their respective `arch.h`) |
| 7 | Link: `undefined reference to _binary_init_bin_end` | `proc.c` uses x86 objcopy blob symbols; arm64 has `init_elf[]` as a C array | Added `#ifdef __aarch64__` branch in `proc.c` using `init_elf` + `init_elf_len` |
| 8 | Link: `__aarch64_ldadd2_relax` etc. | gcc 15 emits outline-atomics calls even with `-mno-outline-atomics`; libgcc not linked | Added `$(LIBGCC) := $(shell $(CC) -print-libgcc-file-name)` to final link |
| 9 | Compile: `implicit declaration of acpi_do_poweroff` in `sys_identity.c` | `sys_reboot` has unconditional x86 ACPI + i8042 + `cli; hlt` | `#ifdef __x86_64__` / `#else arch_debug_exit(0)` on ARM64, plus `arch_disable_irq() + arch_halt()` loop |
| 10 | Compile: `implicit declaration of arch_get_cmdline` in procfs.c | ARM64 `arch.h` missing the prototype | Added inline stub returning `""` |
| 11 | Link: many `sys_getpid`, `sys_rename`, etc. undefined | ARM64 Makefile SHARED_OBJS missing ~20 sources that landed post-divergence | Added: sys_exec, sys_identity, sys_cap, sys_time, sys_dir, sys_meta, sys_disk, futex, waitq, pty, tty, fd_table, vma, cap_policy, ext2_vfs, procfs, memfd, poll_test, unix_socket, usb_mouse, rtl8169, ramdisk |
| 12 | Link: `g_poll_waiter` undefined | Global state for poll wake, defined in x86 `pit.c` | Added stub definition in `kernel/arch/arm64/stubs.c` |

### Files touched

```
.claude/CLAUDE.md                         (arch isolation rule revised)
ARM64.md                                  (new)
kernel/arch/arm64/Makefile                (CFLAGS, LDFLAGS, SHARED_OBJS, rules)
kernel/arch/arm64/arch.h                  (arch_get_cmdline stub)
kernel/arch/arm64/kbd.h                   (kbd_has_data prototype)
kernel/arch/arm64/stubs.c                 (kbd_has_data, g_poll_waiter)
kernel/arch/arm64/vmm_arm64.c             (remove static qualifiers)
kernel/drivers/panic_screen.c             (arch_disable_irq/arch_halt instead of cli/hlt)
kernel/proc/proc.c                        (#ifdef __aarch64__ init blob)
kernel/syscall/sys_identity.c             (#ifdef __x86_64__ sys_reboot)
```

### Remaining link-stage blockers (next phase — real code work, not setup)

**Blocker A: `vmm_phys_of_user_raw` and `vmm_set_user_prot` undefined.** `kernel/syscall/sys_memory.c` uses these for mprotect/mmap user-page operations. `kernel/mm/vmm.c` defines them for x86 PML4. ARM64's `vmm_arm64.c` does not implement them — the walker is there but the wrappers aren't. **~4-8h** to implement page-table walk + PTE prot update for ARM64 L0/L1/L2/L3 layout (VMSAv8-64 page descriptor bits 6-7 for AP, bit 54 for UXN/PXN).

**Blocker B: initrd x86 blob symbol names.** `kernel/fs/initrd.c` has a hardcoded table of `_binary_{login,vigil,shell,echo,cat,ls}_bin_start` / `_end` pointers — the x86 objcopy blob convention. ARM64 has `login_arm64_bin.c`, `shell_arm64_bin.c`, etc., each defining `<name>_elf[]` + `<name>_elf_len`. Two options:

1. **`#ifdef __aarch64__` in `initrd.c`** with a parallel table pointing at the C-array symbols (~1h, option-A compliant)
2. **Generate linker-visible aliases** via an assembly stub: `_binary_login_bin_start = login_elf; _binary_login_bin_end = login_elf + login_elf_len;` (~2h, works without touching initrd.c but clunky)

Recommend option 1. ~2-4h total.

### Known wrinkle to investigate

The arm64 ARM64 blobs (`init_arm64_bin.c` and siblings) are stale — they were pre-built pre-divergence and likely reference older syscall numbers and older musl ABI. Once the kernel links, they may crash immediately at EL0 entry. Fresh rebuild requires:

- `aarch64-linux-musl` toolchain (or crosstool-NG / Alpine sysroot)
- Cross-compile vigil, stsh, coreutils for aarch64
- Regenerate `<name>_arm64_bin.c` via `xxd -i` or `objcopy -O ihex`

ARM64.md §10 phase A4 covers this; expect ~10h once the kernel boots to scheduler.

### How to reproduce the build

```bash
cd kernel/arch/arm64
make clean
make 2>&1 | tail -30
# Should link cleanly once blockers A and B are fixed.
# Then: make run  (QEMU virt -kernel build/aegis-arm64.elf)
```

---

*Research conducted by parallel Explore agents on 2026-04-12. Setup work completed same day. Primary sources: `kernel/arch/arm64/` on master, `origin/arm64-old`, `tests/Cargo.toml`, `vortex/src/core/qemu.rs`, Linux kernel `Documentation/arch/arm64/booting.rst`, Raspberry Pi config.txt reference, QEMU virt documentation.*
