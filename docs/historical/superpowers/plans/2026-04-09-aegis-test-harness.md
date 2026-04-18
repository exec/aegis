# Aegis Test Harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the Aegis-side test infrastructure on top of Vortex's QemuBackend — a Rust test crate with assertion helpers, machine presets, and a boot oracle that replaces the current no-op `make test`.

**Architecture:** A standalone Rust crate at `tests/Cargo.toml` depends on Vortex as a library (features=["qemu"]). `AegisHarness::boot(opts, iso)` spawns QEMU via `QemuBackend::spawn()`, collects serial output with a 30s timeout, and returns `ConsoleOutput`. Assertion functions operate on `ConsoleOutput` and panic with full captured output on failure. A `.vortex/config.toml` is added separately for multi-stage stack scenarios.

**Tech Stack:** Rust, Tokio, Vortex library (`path = "../../vortex"`, feature `qemu`), `qemu-system-x86_64`

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `tests/Cargo.toml` | Create | Crate manifest, vortex path dependency |
| `tests/src/lib.rs` | Create | Re-exports all public API |
| `tests/src/presets.rs` | Create | `aegis_pc()`, `aegis_q35()`, `iso()`, `disk()` |
| `tests/src/assert.rs` | Create | All assertion functions + `WaitTimeout` |
| `tests/src/harness.rs` | Create | `AegisHarness`, `HarnessError` |
| `tests/tests/boot_oracle.rs` | Create | First integration test — ports boot.txt |
| `.vortex/config.toml` | Create | Machine templates + install-test stack |
| `Makefile` | Modify | Replace placeholder `test:`, add `test-q35:`, `install-test:` |

---

## Task 1: Scaffold the crate

**Files:**
- Create: `tests/Cargo.toml`
- Create: `tests/src/lib.rs`

- [ ] **Step 1: Create `tests/Cargo.toml`**

```toml
[package]
name = "aegis-tests"
version = "0.1.0"
edition = "2021"

[dependencies]
vortex = { path = "../../vortex", features = ["qemu"] }
tokio = { version = "1", features = ["full"] }
chrono = "0.4"
```

- [ ] **Step 2: Create `tests/src/lib.rs`** (skeleton — filled in by later tasks)

```rust
pub mod assert;
pub mod harness;
pub mod presets;
```

- [ ] **Step 3: Create placeholder module files so the crate compiles**

`tests/src/presets.rs`:
```rust
// filled in Task 2
```

`tests/src/harness.rs`:
```rust
// filled in Task 4
```

`tests/src/assert.rs`:
```rust
// filled in Task 3
```

- [ ] **Step 4: Verify it compiles**

```bash
cargo check --manifest-path tests/Cargo.toml
```

Expected: no errors (empty modules compile fine)

- [ ] **Step 5: Commit**

```bash
git add tests/Cargo.toml tests/src/
git commit -m "feat(tests): scaffold aegis-tests crate"
```

---

## Task 2: Machine presets

**Files:**
- Modify: `tests/src/presets.rs`

- [ ] **Step 1: Write the failing unit tests**

Replace `tests/src/presets.rs` with:

```rust
use std::path::PathBuf;
use vortex::core::config::QemuOpts;

pub fn iso() -> PathBuf {
    let val = std::env::var("AEGIS_ISO").unwrap_or_else(|_| "build/aegis.iso".into());
    PathBuf::from(val)
}

pub fn disk() -> PathBuf {
    let val = std::env::var("AEGIS_DISK").unwrap_or_else(|_| "build/disk.img".into());
    PathBuf::from(val)
}

pub fn aegis_pc() -> QemuOpts {
    todo!()
}

pub fn aegis_q35() -> QemuOpts {
    todo!()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pc_preset_machine_and_serial() {
        let opts = aegis_pc();
        assert_eq!(opts.machine, "pc");
        assert!(opts.serial_capture);
        assert_eq!(opts.display, "none");
    }

    #[test]
    fn pc_preset_has_isa_debug_exit() {
        let opts = aegis_pc();
        let args = opts.extra_args.join(" ");
        assert!(args.contains("isa-debug-exit"), "missing isa-debug-exit in: {args}");
        assert!(args.contains("0xf4"), "missing iobase in: {args}");
    }

    #[test]
    fn pc_preset_no_devices() {
        let opts = aegis_pc();
        assert!(opts.devices.is_empty());
        assert!(opts.drives.is_empty());
    }

    #[test]
    fn q35_preset_machine_and_serial() {
        let opts = aegis_q35();
        assert_eq!(opts.machine, "q35");
        assert!(opts.serial_capture);
    }

    #[test]
    fn q35_preset_has_nvme_and_xhci() {
        let opts = aegis_q35();
        let devices = opts.devices.join(" ");
        assert!(devices.contains("qemu-xhci"), "missing xhci in: {devices}");
        assert!(devices.contains("nvme"), "missing nvme in: {devices}");
        assert!(devices.contains("virtio-net-pci"), "missing virtio-net in: {devices}");
    }

    #[test]
    fn q35_preset_has_drive() {
        let opts = aegis_q35();
        assert!(!opts.drives.is_empty(), "q35 must have at least one drive");
        assert!(opts.drives[0].contains("nvme0"), "drive must be named nvme0");
    }
}
```

- [ ] **Step 2: Run — verify tests fail**

```bash
cargo test --manifest-path tests/Cargo.toml -- presets 2>&1 | tail -5
```

Expected: compile errors (`todo!()` panics or missing impl)

- [ ] **Step 3: Implement `aegis_pc()` and `aegis_q35()`**

Replace the `todo!()` stubs:

```rust
pub fn aegis_pc() -> QemuOpts {
    QemuOpts {
        machine: "pc".into(),
        display: "none".into(),
        devices: vec![],
        drives: vec![],
        extra_args: vec![
            "-vga".into(), "std".into(),
            "-no-reboot".into(),
            "-device".into(), "isa-debug-exit,iobase=0xf4,iosize=0x04".into(),
        ],
        serial_capture: true,
    }
}

pub fn aegis_q35() -> QemuOpts {
    QemuOpts {
        machine: "q35".into(),
        display: "none".into(),
        devices: vec![
            "qemu-xhci,id=xhci".into(),
            "usb-kbd,bus=xhci.0".into(),
            "usb-mouse,bus=xhci.0".into(),
            "nvme,drive=nvme0,serial=aegis0".into(),
            "virtio-net-pci,netdev=net0".into(),
        ],
        drives: vec![
            format!("file={},format=raw,if=none,id=nvme0", disk().display()),
        ],
        extra_args: vec![
            "-netdev".into(),
            "user,id=net0,hostfwd=tcp::2222-:22,hostfwd=tcp::8080-:80".into(),
            "-vga".into(), "std".into(),
            "-no-reboot".into(),
            "-device".into(), "isa-debug-exit,iobase=0xf4,iosize=0x04".into(),
        ],
        serial_capture: true,
    }
}
```

- [ ] **Step 4: Run — verify tests pass**

```bash
cargo test --manifest-path tests/Cargo.toml -- presets
```

Expected: `test presets::tests::pc_preset_machine_and_serial ... ok` (and 4 others)

- [ ] **Step 5: Commit**

```bash
git add tests/src/presets.rs
git commit -m "feat(tests): aegis_pc and aegis_q35 presets"
```

---

## Task 3: Assertion API

**Files:**
- Modify: `tests/src/assert.rs`

- [ ] **Step 1: Write failing tests**

Replace `tests/src/assert.rs` with:

```rust
use std::time::Duration;
use vortex::{ConsoleOutput, ConsoleStream};

// ── Collected-output assertions ─────────────────────────────────────────────

pub fn assert_subsystem_ok(out: &ConsoleOutput, subsystem: &str) {
    todo!()
}

pub fn assert_subsystem_fail(out: &ConsoleOutput, subsystem: &str) {
    todo!()
}

pub fn assert_boot_subsequence(out: &ConsoleOutput, expected: &[&str]) {
    todo!()
}

pub fn assert_line_contains(out: &ConsoleOutput, substr: &str) {
    todo!()
}

pub fn assert_no_line_contains(out: &ConsoleOutput, substr: &str) {
    todo!()
}

// ── Streaming assertion ──────────────────────────────────────────────────────

#[derive(Debug)]
pub struct WaitTimeout {
    pub pattern: String,
}

impl std::fmt::Display for WaitTimeout {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "wait_for_line: timed out waiting for {:?}", self.pattern)
    }
}

pub async fn wait_for_line(
    stream: &mut ConsoleStream,
    pattern: &str,
    timeout: Duration,
) -> Result<String, WaitTimeout> {
    todo!()
}

// ── Tests ────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn out(kernel_lines: &[&str]) -> ConsoleOutput {
        ConsoleOutput {
            lines: kernel_lines.iter().map(|s| s.to_string()).collect(),
            kernel_lines: kernel_lines.iter().map(|s| s.to_string()).collect(),
        }
    }

    // assert_subsystem_ok

    #[test]
    fn subsystem_ok_passes_when_present() {
        let o = out(&["[PMM] OK: 512MB mapped", "[VMM] OK: paging"]);
        assert_subsystem_ok(&o, "PMM"); // must not panic
    }

    #[test]
    #[should_panic(expected = "assert_subsystem_ok(\"PMM\")")]
    fn subsystem_ok_panics_when_absent() {
        let o = out(&["[VMM] OK: paging"]);
        assert_subsystem_ok(&o, "PMM");
    }

    // assert_subsystem_fail

    #[test]
    fn subsystem_fail_passes_when_present() {
        let o = out(&["[EXT2] FAIL: bad magic"]);
        assert_subsystem_fail(&o, "EXT2");
    }

    #[test]
    #[should_panic(expected = "assert_subsystem_fail(\"EXT2\")")]
    fn subsystem_fail_panics_when_absent() {
        let o = out(&["[EXT2] OK: mounted"]);
        assert_subsystem_fail(&o, "EXT2");
    }

    // assert_boot_subsequence

    #[test]
    fn subsequence_passes_exact() {
        let o = out(&["[PMM] OK", "[VMM] OK", "[SCHED] OK"]);
        assert_boot_subsequence(&o, &["[PMM] OK", "[VMM] OK", "[SCHED] OK"]);
    }

    #[test]
    fn subsequence_passes_with_gaps() {
        let o = out(&["[PMM] OK", "[NOISE]", "[VMM] OK", "[MORE NOISE]", "[SCHED] OK"]);
        assert_boot_subsequence(&o, &["[PMM] OK", "[VMM] OK", "[SCHED] OK"]);
    }

    #[test]
    #[should_panic(expected = "assert_boot_subsequence")]
    fn subsequence_fails_wrong_order() {
        let o = out(&["[VMM] OK", "[PMM] OK"]);
        assert_boot_subsequence(&o, &["[PMM] OK", "[VMM] OK"]);
    }

    #[test]
    #[should_panic(expected = "assert_boot_subsequence")]
    fn subsequence_fails_missing_line() {
        let o = out(&["[PMM] OK", "[VMM] OK"]);
        assert_boot_subsequence(&o, &["[PMM] OK", "[VMM] OK", "[SCHED] OK"]);
    }

    // assert_line_contains

    #[test]
    fn line_contains_passes() {
        let o = out(&["[NET] configured: 10.0.2.15"]);
        assert_line_contains(&o, "[NET] configured:");
    }

    #[test]
    #[should_panic(expected = "assert_line_contains")]
    fn line_contains_panics_when_absent() {
        let o = out(&["[PMM] OK"]);
        assert_line_contains(&o, "[NET] configured:");
    }

    // assert_no_line_contains

    #[test]
    fn no_line_contains_passes_when_absent() {
        let o = out(&["[PMM] OK", "[VMM] OK"]);
        assert_no_line_contains(&o, "[NET]");
    }

    #[test]
    #[should_panic(expected = "assert_no_line_contains")]
    fn no_line_contains_panics_when_present() {
        let o = out(&["[PMM] OK", "[NET] configured: 10.0.2.15"]);
        assert_no_line_contains(&o, "[NET]");
    }
}
```

- [ ] **Step 2: Run — verify tests fail**

```bash
cargo test --manifest-path tests/Cargo.toml -- assert 2>&1 | tail -10
```

Expected: panics at `todo!()` in each test

- [ ] **Step 3: Implement the assertion functions**

Replace the `todo!()` stubs:

```rust
pub fn assert_subsystem_ok(out: &ConsoleOutput, subsystem: &str) {
    let prefix = format!("[{}] OK", subsystem);
    if !out.kernel_contains(&prefix) {
        panic!(
            "assert_subsystem_ok({:?}): not found in kernel output\n\nCapture:\n{}",
            subsystem,
            out.kernel_lines.join("\n")
        );
    }
}

pub fn assert_subsystem_fail(out: &ConsoleOutput, subsystem: &str) {
    let prefix = format!("[{}] FAIL", subsystem);
    if !out.kernel_contains(&prefix) {
        panic!(
            "assert_subsystem_fail({:?}): not found in kernel output\n\nCapture:\n{}",
            subsystem,
            out.kernel_lines.join("\n")
        );
    }
}

pub fn assert_boot_subsequence(out: &ConsoleOutput, expected: &[&str]) {
    let mut matched = 0;
    for line in &out.kernel_lines {
        if matched == expected.len() {
            break;
        }
        if line.contains(expected[matched]) {
            matched += 1;
        }
    }
    if matched < expected.len() {
        panic!(
            "assert_boot_subsequence: matched {}/{} lines; first missing: {:?}\n\nCapture:\n{}",
            matched,
            expected.len(),
            expected[matched],
            out.kernel_lines.join("\n")
        );
    }
}

pub fn assert_line_contains(out: &ConsoleOutput, substr: &str) {
    if !out.contains(substr) {
        panic!(
            "assert_line_contains({:?}): not found in output\n\nCapture:\n{}",
            substr,
            out.lines.join("\n")
        );
    }
}

pub fn assert_no_line_contains(out: &ConsoleOutput, substr: &str) {
    if let Some(line) = out.lines.iter().find(|l| l.contains(substr)) {
        panic!(
            "assert_no_line_contains({:?}): found unexpected line: {:?}\n\nCapture:\n{}",
            substr,
            line,
            out.lines.join("\n")
        );
    }
}

pub async fn wait_for_line(
    stream: &mut ConsoleStream,
    pattern: &str,
    timeout: Duration,
) -> Result<String, WaitTimeout> {
    let deadline = tokio::time::Instant::now() + timeout;
    loop {
        match tokio::time::timeout_at(deadline, stream.next_line()).await {
            Ok(Some(line)) if line.contains(pattern) => return Ok(line),
            Ok(Some(_)) => continue,
            Ok(None) | Err(_) => {
                return Err(WaitTimeout {
                    pattern: pattern.to_string(),
                })
            }
        }
    }
}
```

- [ ] **Step 4: Run — verify all assertion tests pass**

```bash
cargo test --manifest-path tests/Cargo.toml -- assert
```

Expected: all 12 tests pass

- [ ] **Step 5: Commit**

```bash
git add tests/src/assert.rs
git commit -m "feat(tests): assertion API — subsystem_ok/fail, subsequence, line_contains, wait_for_line"
```

---

## Task 4: AegisHarness

**Files:**
- Modify: `tests/src/harness.rs`
- Modify: `tests/src/lib.rs`

- [ ] **Step 1: Write the harness**

Replace `tests/src/harness.rs`:

```rust
use std::path::Path;
use std::time::Duration;
use std::collections::HashMap;
use std::sync::Arc;

use chrono::Utc;
use vortex::{
    ConsoleOutput, ConsoleStream, QemuBackend, QemuProcess, ResourceLimits,
    VmInstance, VmSpec, VmState,
};
use vortex::core::config::{BootSource, ExitMapping, ExitMeaning, QemuOpts};

#[derive(Debug)]
pub enum HarnessError {
    /// Kernel explicitly signalled failure via isa-debug-exit (exit code 35).
    KernelFail(ConsoleOutput),
    /// QEMU failed to start.
    SpawnError(String),
}

impl std::fmt::Display for HarnessError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            HarnessError::KernelFail(out) => write!(
                f,
                "kernel signalled FAIL via isa-debug-exit\n\nCapture:\n{}",
                out.kernel_lines.join("\n")
            ),
            HarnessError::SpawnError(msg) => write!(f, "QEMU spawn error: {msg}"),
        }
    }
}

fn boot_timeout() -> Duration {
    let secs = std::env::var("AEGIS_BOOT_TIMEOUT")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(30u64);
    Duration::from_secs(secs)
}

fn make_vm(opts: QemuOpts, iso: &Path) -> VmInstance {
    VmInstance {
        id: "aegis-test".into(),
        spec: VmSpec {
            image: String::new(),
            memory: 2048,
            cpus: 1,
            ports: HashMap::new(),
            volumes: HashMap::new(),
            environment: HashMap::new(),
            command: None,
            labels: HashMap::new(),
            network_config: None,
            resource_limits: ResourceLimits::default(),
            backend: Some("qemu".into()),
            boot_source: Some(BootSource::Cdrom { iso: iso.to_path_buf() }),
            qemu_opts: Some(opts),
            exit_mappings: vec![
                ExitMapping { raw: 33, meaning: ExitMeaning::Pass },
                ExitMapping { raw: 35, meaning: ExitMeaning::Fail },
            ],
        },
        state: VmState::Stopped,
        backend: Arc::new(QemuBackend::new()),
        created_at: Utc::now(),
        updated_at: Utc::now(),
    }
}

pub struct AegisHarness;

impl AegisHarness {
    /// Boot QEMU, collect all serial output, return when either QEMU exits or
    /// AEGIS_BOOT_TIMEOUT (default 30s) elapses. QEMU is killed on timeout.
    pub async fn boot(opts: QemuOpts, iso: &Path) -> Result<ConsoleOutput, HarnessError> {
        let vm = make_vm(opts, iso);
        let backend = QemuBackend::new();
        let mut proc = backend
            .spawn(&vm)
            .map_err(|e| HarnessError::SpawnError(e.to_string()))?;

        let mut stream = proc
            .console
            .take()
            .expect("serial_capture must be true in QemuOpts");

        let deadline = tokio::time::Instant::now() + boot_timeout();
        let mut output = ConsoleOutput::default();

        loop {
            match tokio::time::timeout_at(deadline, stream.next_line()).await {
                Ok(Some(line)) => {
                    if line.starts_with('[') {
                        output.kernel_lines.push(line.clone());
                    }
                    output.lines.push(line);
                }
                Ok(None) => break,   // QEMU exited
                Err(_) => {          // deadline hit
                    let _ = proc.kill().await;
                    break;
                }
            }
        }

        // Reap process; check for explicit kernel failure signal.
        if let Ok(status) = proc.wait().await {
            if let Some(ExitMeaning::Fail) = status.apply_mappings(&vm.spec.exit_mappings) {
                return Err(HarnessError::KernelFail(output));
            }
        }

        Ok(output)
    }

    /// Boot QEMU and return live stream + process handle.
    /// Caller is responsible for killing the process after use.
    pub async fn boot_stream(
        opts: QemuOpts,
        iso: &Path,
    ) -> Result<(ConsoleStream, QemuProcess), HarnessError> {
        let vm = make_vm(opts, iso);
        let backend = QemuBackend::new();
        let mut proc = backend
            .spawn(&vm)
            .map_err(|e| HarnessError::SpawnError(e.to_string()))?;
        let stream = proc
            .console
            .take()
            .expect("serial_capture must be true in QemuOpts");
        Ok((stream, proc))
    }
}
```

- [ ] **Step 2: Update `tests/src/lib.rs` to re-export public API**

```rust
pub mod assert;
pub mod harness;
pub mod presets;

pub use assert::{
    assert_boot_subsequence, assert_line_contains, assert_no_line_contains,
    assert_subsystem_fail, assert_subsystem_ok, wait_for_line, WaitTimeout,
};
pub use harness::{AegisHarness, HarnessError};
pub use presets::{aegis_pc, aegis_q35, disk, iso};
```

- [ ] **Step 3: Verify it compiles**

```bash
cargo check --manifest-path tests/Cargo.toml
```

Expected: no errors

- [ ] **Step 4: Commit**

```bash
git add tests/src/harness.rs tests/src/lib.rs
git commit -m "feat(tests): AegisHarness — boot, boot_stream, HarnessError"
```

---

## Task 5: Boot oracle integration test

**Files:**
- Create: `tests/tests/boot_oracle.rs`

- [ ] **Step 1: Create the integration test**

```rust
// tests/tests/boot_oracle.rs
//
// Integration test — requires a built ISO.
// Run: AEGIS_ISO=build/aegis.iso cargo test --manifest-path tests/Cargo.toml boot_oracle

use aegis_tests::{aegis_pc, iso, AegisHarness, assert_boot_subsequence, assert_no_line_contains};

/// Lines from tests/historical/expected/boot.txt expressed as a subsequence.
/// Gaps are allowed (extra output is fine); order is enforced.
const BOOT_ORACLE: &[&str] = &[
    "[SERIAL] OK: COM1 initialized",
    "[VGA] OK: text mode",
    "[PMM] OK:",
    "[VMM] OK: kernel mapped",
    "[VMM] OK: mapped-window allocator",
    "[KVA] OK:",
    "[CAP] OK: capability subsystem",
    "[IDT] OK:",
    "[PIC] OK:",
    "[PIT] OK:",
    "[KBD] OK:",
    "[GDT] OK:",
    "[TSS] OK:",
    "[SYSCALL] OK:",
    "[SMAP] OK:",
    "[SMEP] OK:",
    "[RAMDISK] OK:",
    "[VFS] OK:",
    "[INITRD] OK:",
    "[ACPI] OK:",
    "[LAPIC] OK:",
    "[IOAPIC] OK:",
    "[EXT2] OK: mounted ramdisk0",
    "[CAP] OK: loaded",
    "[SMP] OK:",
    "[VMM] OK: identity map removed",
    "[SCHED] OK:",
];

#[tokio::test]
async fn boot_oracle() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found — run `make iso` first", iso.display());
        return;
    }
    let out = AegisHarness::boot(aegis_pc(), &iso)
        .await
        .expect("QEMU failed to start");
    assert_boot_subsequence(&out, BOOT_ORACLE);
}

#[tokio::test]
async fn no_net_lines_on_pc() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found — run `make iso` first", iso.display());
        return;
    }
    let out = AegisHarness::boot(aegis_pc(), &iso)
        .await
        .expect("QEMU failed to start");
    // -machine pc has no virtio-net; net init lines must not appear
    assert_no_line_contains(&out, "[NET] configured:");
    assert_no_line_contains(&out, "[NET] ICMP:");
}
```

- [ ] **Step 2: Verify it compiles**

```bash
cargo test --manifest-path tests/Cargo.toml --no-run 2>&1 | tail -5
```

Expected: compiles without errors

- [ ] **Step 3: If `build/aegis.iso` exists, run the oracle**

```bash
AEGIS_ISO=build/aegis.iso cargo test --manifest-path tests/Cargo.toml boot_oracle -- --nocapture
```

Expected: both tests pass, or SKIP messages if no ISO.

- [ ] **Step 4: Commit**

```bash
git add tests/tests/boot_oracle.rs
git commit -m "feat(tests): boot oracle — ports historical boot.txt as subsequence test"
```

---

## Task 6: `.vortex/config.toml`

**Files:**
- Create: `.vortex/config.toml`

- [ ] **Step 1: Create the config**

```toml
default_backend = "qemu"

# ── aegis-pc ─────────────────────────────────────────────────────────────────
# Base boot: kernel subsystems, no NVMe/USB/net.
# Maps to the aegis_pc() Rust preset.

[templates.aegis-pc]
description = "Aegis base boot — kernel + subsystem tests"
memory = 2048
cpus = 1

[templates.aegis-pc.boot_source]
type = "cdrom"
iso = "build/aegis.iso"

[templates.aegis-pc.qemu]
machine = "pc"
display = "none"
serial_capture = true
extra_args = ["-vga", "std", "-no-reboot",
              "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]

[[templates.aegis-pc.exit_mappings]]
raw = 33
meaning = "pass"

[[templates.aegis-pc.exit_mappings]]
raw = 35
meaning = "fail"

# ── aegis-q35 ────────────────────────────────────────────────────────────────
# Full hardware stack: NVMe, xHCI, virtio-net.
# Maps to the aegis_q35() Rust preset.

[templates.aegis-q35]
description = "Aegis q35 — NVMe, xHCI, virtio-net, network stack"
memory = 2048
cpus = 1

[templates.aegis-q35.boot_source]
type = "cdrom"
iso = "build/aegis.iso"

[templates.aegis-q35.qemu]
machine = "q35"
display = "none"
serial_capture = true
devices = [
  "qemu-xhci,id=xhci",
  "usb-kbd,bus=xhci.0",
  "usb-mouse,bus=xhci.0",
  "nvme,drive=nvme0,serial=aegis0",
  "virtio-net-pci,netdev=net0",
]
drives = ["file=build/disk.img,format=raw,if=none,id=nvme0"]
extra_args = [
  "-netdev", "user,id=net0,hostfwd=tcp::2222-:22,hostfwd=tcp::8080-:80",
  "-vga", "std",
  "-no-reboot",
  "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
]

[[templates.aegis-q35.exit_mappings]]
raw = 33
meaning = "pass"

[[templates.aegis-q35.exit_mappings]]
raw = 35
meaning = "fail"

# ── aegis-install-test ───────────────────────────────────────────────────────
# Multi-stage: boot ISO → installer writes to disk → reboot from NVMe → verify.
# Run with: vortex stack up aegis-install-test

[stacks.aegis-install-test]
description = "Boot ISO, install to NVMe, reboot from disk, verify"

[[stacks.aegis-install-test.stages]]
name = "install"
template = "aegis-q35"
disk_artifact = { create = "512M", id = "rootdisk" }

[[stacks.aegis-install-test.stages]]
name = "verify"
template = "aegis-q35"
boot_source = { type = "disk", path = "${rootdisk}" }
```

- [ ] **Step 2: Verify vortex can parse it**

```bash
vortex template list 2>&1 | grep -E "aegis|error"
```

Expected: `aegis-pc` and `aegis-q35` listed, no parse errors

- [ ] **Step 3: Commit**

```bash
git add .vortex/config.toml
git commit -m "feat(tests): vortex config — aegis-pc, aegis-q35, install-test stack"
```

---

## Task 7: Makefile

**Files:**
- Modify: `Makefile` lines 404-407

- [ ] **Step 1: Replace the placeholder test target**

Find and replace the current `test:` block:

Old:
```makefile
# ── Test (placeholder — test suite being rebuilt) ────────────────────────────
test:
	@echo "Test suite is being rebuilt. Historical tests in tests/historical/"
	@echo "Run individual historical tests with: python3 tests/historical/test_*.py"
```

New:
```makefile
# ── Tests ─────────────────────────────────────────────────────────────────────
test: iso
	cargo test --manifest-path tests/Cargo.toml -- --nocapture

test-q35: iso disk
	AEGIS_PRESET=q35 cargo test --manifest-path tests/Cargo.toml -- --nocapture

install-test: iso disk
	vortex stack up aegis-install-test
```

- [ ] **Step 2: Verify `make test` runs (will skip oracle if no ISO built)**

```bash
make test 2>&1 | tail -10
```

Expected: cargo test runs; oracle tests print SKIP if no ISO; unit tests (presets, assert) pass.

- [ ] **Step 3: Commit**

```bash
git add Makefile
git commit -m "feat(tests): make test runs aegis-tests crate; add test-q35 and install-test targets"
```
