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
