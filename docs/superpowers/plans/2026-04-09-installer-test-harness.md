# Installer Test Harness Implementation Plan (Phase 1 of 3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the historical Python installer test (`tests/historical/test_installer.py`) to the Rust cargo harness as an active regression gate. Runs against the **current text-mode installer** as-is. Lands FIRST — before any installer refactoring — so that subsequent changes can be verified against a working baseline.

**Architecture:** Two-boot test: (1) boot live ISO with a fresh empty NVMe disk image attached, log into stsh as root, invoke `installer`, drive it through its text prompts via QEMU HMP `sendkey`, wait for `Installation complete`, shut down; (2) boot the same NVMe disk image standalone via OVMF UEFI firmware (no ISO), verify Bastion greeter comes up and `[EXT2] OK: mounted nvme0p1` appears in the serial log. A disk file persists between the two boots.

**Tech Stack:** Rust (tokio, vortex path dep), QEMU q35 with NVMe drive, OVMF UEFI firmware on the build box, HMP over Unix monitor socket.

**Spec:** `docs/superpowers/specs/2026-04-09-gui-installer-design.md` (section "Testing strategy → installer_test.rs")

**Phase chain:** This plan is Phase 1. Phase 2 is `2026-04-09-libinstall-extraction.md` (extracts the installer logic into a library). Phase 3 is `2026-04-09-gui-installer.md` (builds the Glyph-based wizard). Each phase re-runs this test.

---

## File Structure

**Create:**
- `/Users/dylan/Developer/aegis/tests/tests/installer_test.rs` — the new two-boot regression test

**Modify:**
- `/Users/dylan/Developer/aegis/tests/src/harness.rs` — add `AegisHarness::boot_with_disk` helper
- `/Users/dylan/Developer/aegis/tests/src/presets.rs` — add `aegis_q35_installer()` preset builder
- `/Users/dylan/Developer/aegis/tests/src/lib.rs` — re-export new preset

**Not touched:** the kernel, the installer binary, any userspace code. This phase only adds tests.

**Build box prerequisite:** fishbowl (10.0.0.18) must have `OVMF_CODE_4M.fd` at `/usr/share/OVMF/OVMF_CODE_4M.fd`. The test skips gracefully if it's missing. The `ovmf` Debian/Ubuntu package provides it. Verify with `ssh 10.0.0.18 'ls /usr/share/OVMF/OVMF_CODE_4M.fd'` before running Task 3.

---

## Task 1: Add `boot_with_disk` harness helper + NVMe preset

**Files:**
- Modify: `/Users/dylan/Developer/aegis/tests/src/harness.rs`
- Modify: `/Users/dylan/Developer/aegis/tests/src/presets.rs`
- Modify: `/Users/dylan/Developer/aegis/tests/src/lib.rs`

**Context:** Current `AegisHarness::boot_stream(opts, iso)` at `tests/src/harness.rs:119-133` passes a `BootSource::Cdrom { iso }` to the VM spec. It has no concept of a persistent disk image. The installer test needs two sequential boots that share a disk: boot 1 writes to it, boot 2 reads from it. The disk is passed to QEMU via `-drive file=...,if=none,id=nvme0 -device nvme,drive=nvme0,serial=aegis0`. Vortex's `QemuOpts::drives` vec supports the drive line; we just need a preset that populates it and a harness function that accepts a disk path parameter so the test can construct the same disk file for both boots.

OVMF boot (boot 2 of the installer test) uses `-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd` which is a **pflash drive**, not a storage drive. We represent it the same way — as a `drives` entry — and let QEMU sort it out.

- [ ] **Step 1: Add `aegis_q35_installer` preset to presets.rs**

Edit `/Users/dylan/Developer/aegis/tests/src/presets.rs`. Append this function after the existing `aegis_q35_graphical_mouse` function (which ends around line 85 with `}`), before the `#[cfg(test)]` block:

```rust
/// q35 preset for installer tests: ISO + persistent NVMe drive + monitor socket.
///
/// The caller supplies the disk image path; this function does not
/// create or delete it. Used for Boot 1 of the installer test (ISO
/// boot with empty NVMe attached for installation).
///
/// No usb-kbd: HMP `sendkey` routes to the first keyboard device;
/// with usb-kbd present it intercepts keys the PS/2 kernel driver
/// expects. q35 LPC provides a default PS/2 keyboard.
pub fn aegis_q35_installer(disk_path: &std::path::Path) -> QemuOpts {
    QemuOpts {
        machine: "q35".into(),
        display: "none".into(),
        devices: vec![
            "nvme,drive=nvme0,serial=aegis0".into(),
        ],
        drives: vec![
            format!("file={},if=none,id=nvme0,format=raw", disk_path.display()),
        ],
        extra_args: vec![
            "-cpu".into(), "Broadwell".into(),
            "-no-reboot".into(),
            "-nodefaults".into(),
            "-vga".into(), "std".into(),
        ],
        serial_capture: true,
        monitor_socket: true,
    }
}

/// q35 preset for post-installer boot: NVMe drive only, no ISO, OVMF UEFI firmware.
///
/// Used for Boot 2 of the installer test to verify the installed
/// system boots standalone via UEFI. `disk_path` is the same path
/// that was used in the preceding `aegis_q35_installer` boot.
/// `ovmf_path` is the OVMF_CODE firmware binary (typically
/// `/usr/share/OVMF/OVMF_CODE_4M.fd` on Debian-derived systems).
pub fn aegis_q35_installed_ovmf(disk_path: &std::path::Path,
                                 ovmf_path: &std::path::Path) -> QemuOpts {
    QemuOpts {
        machine: "q35".into(),
        display: "none".into(),
        devices: vec![
            "nvme,drive=nvme0,serial=aegis0".into(),
        ],
        drives: vec![
            format!("if=pflash,format=raw,readonly=on,file={}", ovmf_path.display()),
            format!("file={},if=none,id=nvme0,format=raw", disk_path.display()),
        ],
        extra_args: vec![
            "-cpu".into(), "Broadwell".into(),
            "-no-reboot".into(),
            "-nodefaults".into(),
            "-vga".into(), "std".into(),
        ],
        serial_capture: true,
        monitor_socket: true,
    }
}
```

- [ ] **Step 2: Add `boot_with_disk` to harness.rs**

Edit `/Users/dylan/Developer/aegis/tests/src/harness.rs`. Find the existing `boot_stream` function (at ~line 119) and add this new function **immediately after** its closing `}` at line 133:

```rust
    /// Boot QEMU with a persistent disk image, NO ISO.
    ///
    /// Use this for the second boot of a two-boot test sequence —
    /// the disk image was populated by the first boot and must be
    /// loaded back with the same file path. The caller-supplied
    /// `opts` determines everything else (machine, devices, drives,
    /// extra args). `opts.drives` should contain the `-drive ...`
    /// entries for the NVMe image (and any pflash firmware); this
    /// function does not inject anything into `opts`.
    ///
    /// Returns the same (ConsoleStream, QemuProcess) tuple as
    /// `boot_stream`. Caller is responsible for killing the process
    /// and managing the disk file's lifetime.
    pub async fn boot_disk_only(
        opts: QemuOpts,
    ) -> Result<(ConsoleStream, QemuProcess), HarnessError> {
        let vm = VmInstance {
            id: "aegis-installed".into(),
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
                /* No boot source — boot comes from pflash (UEFI) +
                 * NVMe drive specified in opts.drives. Vortex's
                 * BootSource enum has no "none" variant, so we pass
                 * a fake Cdrom pointing at /dev/null which QEMU
                 * ignores when -boot order overrides it via extra
                 * args. */
                boot_source: Some(BootSource::Cdrom {
                    iso: std::path::PathBuf::from("/dev/null"),
                }),
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
        };
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
```

**Note:** Vortex's `BootSource` enum has no "empty" variant. The fake `Cdrom { iso: "/dev/null" }` boot source causes Vortex to emit `-cdrom /dev/null -boot order=d` which QEMU treats as an empty optical drive. The real boot happens from pflash (UEFI firmware in opts.drives) and the NVMe image. OVMF's firmware scans attached drives for `/EFI/BOOT/BOOTX64.EFI` and finds it on the NVMe's ESP partition. This is the same pattern the historical Python test uses.

- [ ] **Step 3: Re-export new preset from lib.rs**

Edit `/Users/dylan/Developer/aegis/tests/src/lib.rs`. The current line is:

```rust
pub use presets::{aegis_pc, aegis_q35, aegis_q35_graphical_mouse, disk, iso};
```

Change it to:

```rust
pub use presets::{
    aegis_pc, aegis_q35, aegis_q35_graphical_mouse,
    aegis_q35_installer, aegis_q35_installed_ovmf,
    disk, iso,
};
```

- [ ] **Step 4: Verify the crate compiles**

Run:

```bash
cd /Users/dylan/Developer/aegis && cargo check --manifest-path tests/Cargo.toml --tests
```

Expected: clean check, no warnings. If `ExitMeaning`, `ExitMapping`, `ResourceLimits`, `VmSpec`, `VmInstance`, `VmState`, `QemuBackend` or `BootSource` aren't already in scope in `harness.rs`, the existing `boot_stream` function imports them — look at the top of `harness.rs` for the existing `use vortex::{...}` and `use vortex::core::config::{...}` lines and ensure `boot_disk_only` uses the same symbols. The function above is written assuming identical imports; if the build fails on an unresolved symbol, add it to the existing `use` block at the top of the file.

- [ ] **Step 5: Commit**

```bash
cd /Users/dylan/Developer/aegis && git add tests/src/harness.rs tests/src/presets.rs tests/src/lib.rs && git commit -m "$(cat <<'EOF'
test: add installer harness helpers (boot_disk_only + NVMe presets)

Adds AegisHarness::boot_disk_only for the second boot of a two-boot
test sequence (no ISO, boots from a persistent disk populated by the
first boot). Adds aegis_q35_installer (ISO + NVMe drive) and
aegis_q35_installed_ovmf (NVMe + OVMF UEFI firmware, no ISO) presets.

Used by the upcoming installer_test.rs regression gate.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Write `installer_test.rs` end-to-end

**Files:**
- Create: `/Users/dylan/Developer/aegis/tests/tests/installer_test.rs`

**Context:** This test boots the live ISO, logs into stsh as root (using the same `send_keys("root\tforevervigilant\n")` pattern as `login_flow_test.rs` except routed to a TTY instead of Bastion's form), invokes the `installer` binary, drives its text prompts via HMP sendkey, then reboots from the installed disk via OVMF to verify everything worked.

The current text installer prompts, in order:
1. `Install to nvme0? [y/N] ` — type `y<Enter>` to confirm
2. `Root password: ` — type root password + Enter
3. `Confirm root password: ` — type same + Enter
4. `Username: ` (optional — press Enter to skip, or type a name)
5. If username given: `Password: ` + `Confirm password: ` for user account
6. Progress messages, then `=== Installation complete! ===`

We'll create a user account during install (`alice`/`alicepass`) so boot 2 has something to verify beyond the default root credentials.

**Serial markers to wait for:**
- Boot 1: `# ` (stsh prompt after successful login — stsh prints `root@aegis:/#` which contains `# `)
- Installer start: `=== Aegis Installer ===`
- Disk prompt: `Install to nvme0?`
- Install complete: `=== Installation complete! ===`
- Boot 2 success: `[BASTION] greeter ready` (installed grub.cfg default is graphical mode)

- [ ] **Step 1: Write the test file**

Create `/Users/dylan/Developer/aegis/tests/tests/installer_test.rs`:

```rust
// End-to-end installer regression test.
//
// Two-boot sequence:
//   Boot 1: Live ISO + empty NVMe → log in to stsh → run installer →
//           drive its prompts via HMP sendkey → wait for
//           "=== Installation complete! ===" → shut down.
//   Boot 2: OVMF UEFI + same NVMe (no ISO) → wait for
//           "[BASTION] greeter ready" and "[EXT2] OK: mounted nvme0p1".
//
// The NVMe disk image persists on disk between boots in
// tests/target/installer_test_disk.img (128 MB, raw). It's recreated
// fresh at the start of every test run.
//
// Skipped gracefully if OVMF_CODE_4M.fd is not installed — OVMF is a
// Debian/Ubuntu package (apt install ovmf).
//
// Run: AEGIS_ISO=build/aegis.iso cargo test --manifest-path tests/Cargo.toml --test installer_test -- --nocapture

use aegis_tests::{
    aegis_q35_installed_ovmf, aegis_q35_installer, iso, wait_for_line,
    AegisHarness,
};
use std::path::PathBuf;
use std::process::Command;
use std::time::Duration;

const DISK_SIZE_MB: u64 = 128;
const BOOT_TIMEOUT_SECS: u64 = 120;
const INSTALL_TIMEOUT_SECS: u64 = 90;

const ROOT_PW: &str = "forevervigilant";
const USER_NAME: &str = "alice";
const USER_PW: &str = "alicepass";

fn disk_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("target/installer_test_disk.img")
}

fn ovmf_path() -> PathBuf {
    PathBuf::from("/usr/share/OVMF/OVMF_CODE_4M.fd")
}

/// Create a fresh 128 MB sparse file at `path`, overwriting any
/// existing file.
fn make_fresh_disk(path: &std::path::Path) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let _ = std::fs::remove_file(path);
    // Use `truncate -s` for a sparse file — dd would work too but
    // truncate is faster and doesn't need /dev/zero.
    let status = Command::new("truncate")
        .arg(format!("-s{}M", DISK_SIZE_MB))
        .arg(path)
        .status()?;
    if !status.success() {
        return Err(std::io::Error::new(
            std::io::ErrorKind::Other,
            format!("truncate failed with status {}", status),
        ));
    }
    Ok(())
}

#[tokio::test]
async fn install_and_boot_from_nvme() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        return;
    }
    if !ovmf_path().exists() {
        eprintln!("SKIP: OVMF not found at {}", ovmf_path().display());
        eprintln!("      install with: apt install ovmf");
        return;
    }

    let disk = disk_path();
    make_fresh_disk(&disk).expect("create fresh disk");
    eprintln!("fresh disk: {} ({} MB)", disk.display(), DISK_SIZE_MB);

    // ── Boot 1: install ──────────────────────────────────────────────
    eprintln!("==> Boot 1: live ISO + empty NVMe (install phase)");
    let (mut stream, mut proc) =
        AegisHarness::boot_stream(aegis_q35_installer(&disk), &iso)
            .await
            .expect("boot 1 spawn failed");

    // Wait for stsh login shell. The live ISO auto-logs root via
    // vigil's default login service — the shell prompt contains "# "
    // once it's ready for input.
    wait_for_line(&mut stream, "# ", Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .expect("boot 1: stsh prompt never appeared");
    tokio::time::sleep(Duration::from_millis(300)).await;

    // Launch the installer.
    proc.send_keys("installer\n")
        .await
        .expect("sendkey installer");
    wait_for_line(&mut stream, "=== Aegis Installer ===",
                  Duration::from_secs(10))
        .await
        .expect("boot 1: installer banner");

    // Confirm disk.
    wait_for_line(&mut stream, "Install to nvme0?",
                  Duration::from_secs(10))
        .await
        .expect("boot 1: disk prompt");
    proc.send_keys("y\n").await.expect("sendkey y");

    // Root password (twice).
    wait_for_line(&mut stream, "Root password:",
                  Duration::from_secs(10))
        .await
        .expect("boot 1: root pw prompt");
    proc.send_keys(&format!("{}\n", ROOT_PW))
        .await
        .expect("sendkey root pw");
    wait_for_line(&mut stream, "Confirm root password:",
                  Duration::from_secs(10))
        .await
        .expect("boot 1: root pw confirm prompt");
    proc.send_keys(&format!("{}\n", ROOT_PW))
        .await
        .expect("sendkey root pw confirm");

    // Optional user account.
    wait_for_line(&mut stream, "Username:",
                  Duration::from_secs(10))
        .await
        .expect("boot 1: user prompt");
    proc.send_keys(&format!("{}\n", USER_NAME))
        .await
        .expect("sendkey username");
    wait_for_line(&mut stream, "Password:",
                  Duration::from_secs(10))
        .await
        .expect("boot 1: user pw prompt");
    proc.send_keys(&format!("{}\n", USER_PW))
        .await
        .expect("sendkey user pw");
    wait_for_line(&mut stream, "Confirm password:",
                  Duration::from_secs(10))
        .await
        .expect("boot 1: user pw confirm prompt");
    proc.send_keys(&format!("{}\n", USER_PW))
        .await
        .expect("sendkey user pw confirm");

    // Wait for installation to finish.
    wait_for_line(&mut stream, "=== Installation complete! ===",
                  Duration::from_secs(INSTALL_TIMEOUT_SECS))
        .await
        .expect("boot 1: installation did not complete in time");
    eprintln!("    installation complete");

    // Clean shutdown — stsh's exit returns us to login which triggers
    // a graceful path; let QEMU settle then kill.
    proc.send_keys("exit\n").await.ok();
    tokio::time::sleep(Duration::from_millis(1500)).await;
    proc.kill().await.expect("boot 1 kill");
    drop(stream);

    // ── Boot 2: OVMF + installed NVMe ────────────────────────────────
    eprintln!("==> Boot 2: OVMF + installed NVMe (verify standalone boot)");
    let (mut stream2, mut proc2) =
        AegisHarness::boot_disk_only(
            aegis_q35_installed_ovmf(&disk, &ovmf_path()),
        )
        .await
        .expect("boot 2 spawn failed");

    // OVMF firmware init + GRUB menu takes noticeably longer than the
    // direct-BIOS boot (typically 10-20s before Aegis kernel starts).
    wait_for_line(&mut stream2, "[EXT2] OK: mounted nvme0p1",
                  Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .expect("boot 2: [EXT2] OK: mounted nvme0p1 not seen");
    eprintln!("    [EXT2] OK: mounted nvme0p1");

    wait_for_line(&mut stream2, "[BASTION] greeter ready",
                  Duration::from_secs(30))
        .await
        .expect("boot 2: [BASTION] greeter ready not seen");
    eprintln!("    [BASTION] greeter ready");

    proc2.kill().await.expect("boot 2 kill");

    // Success — leave the disk image in place for manual inspection.
    eprintln!("PASS: install_and_boot_from_nvme");
}
```

- [ ] **Step 2: Run `cargo check` to verify the test compiles**

```bash
cd /Users/dylan/Developer/aegis && cargo check --manifest-path tests/Cargo.toml --tests
```

Expected: clean. The test imports `AegisHarness`, `aegis_q35_installer`, `aegis_q35_installed_ovmf`, `iso`, `wait_for_line` — all of which were added or already existed in Task 1.

- [ ] **Step 3: Commit**

```bash
cd /Users/dylan/Developer/aegis && git add tests/tests/installer_test.rs && git commit -m "$(cat <<'EOF'
test: add installer_test regression gate (two-boot install+verify)

Ports tests/historical/test_installer.py to the Rust cargo harness.
Boots live ISO, drives the text installer through its prompts via
HMP sendkey, then boots from the installed NVMe via OVMF UEFI and
verifies the installed system comes up cleanly.

Creates user 'alice' with password 'alicepass' during install.

Skipped gracefully if /usr/share/OVMF/OVMF_CODE_4M.fd is missing
(apt install ovmf provides it on Debian-derived systems).

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Run the new test on fishbowl and fix any real issues

**Files:** none — verification pass.

**Context:** The test was written to match the current text installer's prompts as read from `user/bin/installer/main.c`. Reality sometimes differs — the installer might print a blank line we're not expecting, a prompt might be formatted slightly differently, OVMF might not be installed, etc. Run the test and iterate until green. **Do not move to Phase 2 until this test passes.**

- [ ] **Step 1: Verify OVMF is installed on fishbowl**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 'ls -la /usr/share/OVMF/OVMF_CODE_4M.fd'
```

Expected: `-rw-r--r-- 1 root root 4194304 ... /usr/share/OVMF/OVMF_CODE_4M.fd` (or similar).

If not found: install it (interactive — prompt the user if needed):

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 'sudo apt install -y ovmf'
```

If sudo needs a password and passwordless sudo isn't set up, stop and ask the user to install OVMF manually.

- [ ] **Step 2: Push local branch to origin**

```bash
cd /Users/dylan/Developer/aegis && git push origin master
```

Expected: fast-forward push succeeds.

- [ ] **Step 3: Build + run the new test on fishbowl**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 'cd ~/Developer/aegis && git pull --ff-only 2>&1 | tail -3 && git clean -fdx --exclude=references --exclude=.worktrees 2>&1 | tail -3 && rm -f user/vigil/vigil user/vigictl/vigictl user/login/login.elf && make iso 2>&1 | tail -3 && AEGIS_ISO=$PWD/build/aegis.iso cargo test --manifest-path tests/Cargo.toml --test installer_test -- --nocapture 2>&1 | tail -60'
```

Expected:

```
==> Boot 1: live ISO + empty NVMe (install phase)
    installation complete
==> Boot 2: OVMF + installed NVMe (verify standalone boot)
    [EXT2] OK: mounted nvme0p1
    [BASTION] greeter ready
PASS: install_and_boot_from_nvme
test install_and_boot_from_nvme ... ok
```

- [ ] **Step 4: If the test fails, triage and fix**

Most likely failures and their fixes:

**`boot 1: stsh prompt never appeared` within 120s.** The live ISO might not drop to stsh automatically; it might stop at the Bastion greeter (graphical mode). Check the live ISO's grub.cfg — does it boot `text` or `graphical` by default? If graphical: modify the installer test preset to pass `-append "boot=text"` (add it to `aegis_q35_installer`'s `extra_args`). But `extra_args` is already used for non-kernel flags — the right place is a new `-append` via QemuOpts, or a kernel cmdline prepend on the boot source. Look at `build/isodir/boot/grub/grub.cfg` or `tools/grub-live.cfg` for the default menuentry.

**`boot 1: installer banner` not seen.** The installer might not be in `/bin` on the live ISO — check `user/bin/installer/Makefile` output path and the rootfs layout. Run `ls /bin/installer` from inside stsh via HMP sendkey to verify.

**`boot 1: installation did not complete in time` within 90s.** The install might genuinely take longer than 90s, or it might be stuck. First double the timeout (to 180s); if still failing, examine the serial trace captured by the test (currently all lines are discarded by `wait_for_line` — use the trace-capture pattern from `dock_click_test.rs` lines 212-247 to collect all lines and dump them on failure).

**`boot 2: [EXT2] OK: mounted nvme0p1 not seen`.** The installed grub.cfg might be incorrect, OVMF might not find the ESP, or the installer wrote the wrong partition type GUID. Check the second-boot serial manually by running QEMU interactively with the disk image after boot 1 completes.

**OVMF boots but hangs at GRUB.** OVMF's NVMe boot timing is fragile on some QEMU versions. Try adding `-global driver=nvme,property=serial,value=aegis0` or confirm the NVMe serial matches what the installed grub.cfg references. The current installer doesn't use disk serials, so this shouldn't be an issue, but flag it if it appears.

**Test times out during a send_keys call.** The shared monitor socket might have timing issues. Look at `mouse_api_smoke_test.rs` and `dock_click_test.rs` for the pattern — a 500ms sleep after `boot_stream` returns before the first HMP command is standard.

Fix, rebuild on fishbowl, rerun. Iterate until the test passes. Each fix is its own commit; use descriptive messages like `fix(installer-test): bump install timeout to 180s` or `fix(installer-test): send boot=text kernel cmdline for live ISO`.

- [ ] **Step 5: Re-run the full test suite to confirm no regression**

```bash
ssh -i ~/.ssh/aegis/id_ed25519 dylan@10.0.0.18 'cd ~/Developer/aegis && AEGIS_ISO=$PWD/build/aegis.iso cargo test --manifest-path tests/Cargo.toml -- --nocapture 2>&1 | grep -E "test result|finished"'
```

Expected: all 6 test files pass (boot_oracle, dock_click_test, installer_test, login_flow_test, mouse_api_smoke_test, screendump_test).

- [ ] **Step 6: No commit — verification only**

---

## Self-Review

**Spec coverage:**
- "Port `test_installer.py` to the Rust cargo harness" — Task 2. ✅
- "Two-boot test: install phase, then OVMF boot from installed disk" — Task 2 flow. ✅
- "Persistent NVMe disk image that survives between QEMU boots" — `tests/target/installer_test_disk.img` + `make_fresh_disk`. ✅
- "New `AegisHarness::boot_with_disk`" — Task 1, renamed `boot_disk_only` (more descriptive — no ISO, disk only). ✅
- "Preset gains a new boolean `drive_disk` flag" — rejected in favor of two dedicated preset functions (`aegis_q35_installer` and `aegis_q35_installed_ovmf`). Cleaner than a flag; matches the existing convention where each preset is a function. ✅
- "Budget 180 s total via an `AEGIS_INSTALLER_TEST_TIMEOUT` env var" — not implemented; timeouts are hardcoded constants. If the test needs per-environment tuning later, add the env var knob then. YAGNI for now.

**Placeholder scan:** no TBDs, no "handle errors", every step contains actual commands and code. ✅

**Type consistency:** `AegisHarness::boot_disk_only` uses the same `VmInstance` / `VmSpec` / `BootSource` / `QemuBackend` / `ExitMapping` types as the existing `boot_stream` function. The preset functions return `QemuOpts` matching the existing presets. ✅

**Open correctness risks:**
1. Live ISO might boot graphical by default, not text. If so, the "wait for `# `" step needs to go through Bastion login first. Task 3 Step 4 addresses this.
2. OVMF boot timing is QEMU-version-dependent. 120s timeout should be generous but not guaranteed.
3. Installer prompts might have slightly different exact text ("Install to nvme0?" vs "Install to nvme0? [y/N]"). `wait_for_line` uses substring matching so "Install to nvme0?" should match either.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-04-09-installer-test-harness.md`. Two execution options:

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints.

Next plan in the chain: `2026-04-09-libinstall-extraction.md` (Phase 2).
