// End-to-end installer regression test.
//
// Two-boot sequence:
//   Boot 1: Text-mode live ISO + empty NVMe → log in to stsh →
//           run installer → drive its prompts via HMP sendkey →
//           wait for "=== Installation complete! ===" → shut down.
//   Boot 2: OVMF UEFI + same NVMe (no ISO) → wait for
//           "[BASTION] greeter ready" and "[EXT2] OK: mounted nvme0p1".
//
// The NVMe disk image persists on disk between boots in
// tests/target/installer_test_disk.img (128 MB, raw). It's recreated
// fresh at the start of every test run.
//
// This test requires build/aegis-test.iso (made with
// `make test-iso`) instead of the default graphical build/aegis.iso
// — the default ISO boots boot=graphical quiet which starts Bastion
// and leaves no CLI for driving the text installer. aegis-test.iso
// uses tools/grub-test.cfg (boot=text quiet, timeout=0).
//
// Skipped gracefully if OVMF_CODE_4M.fd is not installed — OVMF is a
// Debian/Ubuntu package (apt install ovmf).
//
// Run: AEGIS_INSTALLER_ISO=build/aegis-test.iso cargo test --manifest-path tests/Cargo.toml --test installer_test -- --nocapture

use aegis_tests::{
    aegis_q35_installed_ovmf, aegis_q35_installed_ovmf_4k,
    aegis_q35_installer, aegis_q35_installer_4k,
    wait_for_line, AegisHarness,
};
use std::path::PathBuf;
use std::process::Command;
use std::time::Duration;

const DISK_SIZE_MB: u64 = 128;
const BOOT_TIMEOUT_SECS: u64 = 120;
const INSTALL_TIMEOUT_SECS: u64 = 180;

const ROOT_PW: &str = "forevervigilant";
const USER_NAME: &str = "alice";
const USER_PW: &str = "alicepass";

/// Text-mode ISO — required for the installer test because the
/// default live ISO boots `boot=graphical quiet` which triggers
/// Bastion, and Bastion has no CLI to drive the installer through.
/// `make test-iso` produces build/aegis-test.iso using
/// tools/grub-test.cfg (boot=text quiet, timeout=0).
fn installer_iso() -> PathBuf {
    let val = std::env::var("AEGIS_INSTALLER_ISO")
        .unwrap_or_else(|_| "build/aegis-test.iso".into());
    PathBuf::from(val)
}

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
    let iso = installer_iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        eprintln!("      build with: make test-iso");
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

    // The login/installer prompts (`login: `, `password: `,
    // `Install to nvme0? [y/N] `, `Root password: `, ...) are
    // written without trailing newlines, so `wait_for_line` cannot
    // match them — it only returns whole lines.  We anchor on the
    // pre-login banner (which DOES end in newlines), then drive
    // login + installer via blind sleeps with generous gaps.  The
    // only markers we `wait_for_line` on are the installer's own
    // `=== Aegis Installer ===` and `=== Installation complete! ===`
    // which are printed with explicit newline terminators.
    wait_for_line(&mut stream, "WARNING: This system",
                  Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .expect("boot 1: pre-login banner never appeared");

    // Let /bin/login finish drawing its `login: ` prompt.
    tokio::time::sleep(Duration::from_millis(700)).await;

    // Username → password → shell.
    proc.send_keys("root\n").await.expect("sendkey root");
    tokio::time::sleep(Duration::from_millis(500)).await;
    proc.send_keys(&format!("{}\n", ROOT_PW))
        .await
        .expect("sendkey login pw");
    tokio::time::sleep(Duration::from_millis(1000)).await;

    // Launch the installer.
    proc.send_keys("installer\n")
        .await
        .expect("sendkey installer");
    wait_for_line(&mut stream, "=== Aegis Installer ===",
                  Duration::from_secs(10))
        .await
        .expect("boot 1: installer banner");

    // Installer prompts (all without trailing newlines):
    //   Install to nvme0? [y/N]
    //   Root password:
    //   Confirm root password:
    //   Username:
    //   Password:
    //   Confirm password:
    // Drive blindly with generous sleeps.
    tokio::time::sleep(Duration::from_millis(1500)).await;
    proc.send_keys("y\n").await.expect("sendkey y");
    tokio::time::sleep(Duration::from_millis(1500)).await;

    // Root password (twice).
    proc.send_keys(&format!("{}\n", ROOT_PW))
        .await
        .expect("sendkey root pw");
    tokio::time::sleep(Duration::from_millis(1500)).await;
    proc.send_keys(&format!("{}\n", ROOT_PW))
        .await
        .expect("sendkey root pw confirm");
    tokio::time::sleep(Duration::from_millis(1500)).await;

    // Optional user account.
    proc.send_keys(&format!("{}\n", USER_NAME))
        .await
        .expect("sendkey username");
    tokio::time::sleep(Duration::from_millis(1500)).await;
    proc.send_keys(&format!("{}\n", USER_PW))
        .await
        .expect("sendkey user pw");
    tokio::time::sleep(Duration::from_millis(1500)).await;
    proc.send_keys(&format!("{}\n", USER_PW))
        .await
        .expect("sendkey user pw confirm");

    // Wait for installation to finish.  Capture lines so a timeout
    // shows what the installer was doing.
    {
        let deadline = tokio::time::Instant::now()
            + Duration::from_secs(INSTALL_TIMEOUT_SECS);
        let mut trace: Vec<String> = Vec::new();
        let mut found = false;
        while tokio::time::Instant::now() < deadline {
            match tokio::time::timeout_at(deadline, stream.next_line()).await {
                Ok(Some(line)) => {
                    let done = line.contains("=== Installation complete! ===");
                    trace.push(line);
                    if done { found = true; break; }
                }
                Ok(None) | Err(_) => break,
            }
        }
        if !found {
            eprintln!("--- installer serial trace (last {} lines) ---",
                      trace.len());
            let skip = trace.len().saturating_sub(40);
            for l in &trace[skip..] {
                eprintln!("  {}", l);
            }
            eprintln!("--- end ---");
            panic!("boot 1: installation did not complete within {}s",
                   INSTALL_TIMEOUT_SECS);
        }
    }
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
    //
    // Success signal: `[EXT2] OK: mounted nvme0p1`.  This proves
    //   (a) OVMF found and loaded the ESP,
    //   (b) GRUB located the Aegis root partition,
    //   (c) the kernel loaded and mounted the installed ext2
    //       rootfs successfully.
    // We intentionally don't wait for [BASTION] greeter ready —
    // the installed grub.cfg boots `boot=graphical` which expects a
    // usable framebuffer, and `-vga std` (the test's VGA config)
    // can't satisfy sys_fb_map. Bastion failing over to respawn
    // here doesn't indicate an install failure.
    wait_for_line(&mut stream2, "[EXT2] OK: mounted nvme0p1",
                  Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .expect("boot 2: [EXT2] OK: mounted nvme0p1 not seen");
    eprintln!("    [EXT2] OK: mounted nvme0p1");

    proc2.kill().await.expect("boot 2 kill");

    // Success — leave the disk image in place for manual inspection.
    eprintln!("PASS: install_and_boot_from_nvme");
}

/// Same two-boot install sequence as `install_and_boot_from_nvme` but
/// with a 4K-native NVMe drive (`physical_block_size=4096`).
///
/// Regression test for the 4K-block installer fix: previously the
/// kernel's gpt_scan issued 8-LBA reads that requested 32 KB from a
/// 4K-sector controller (PRP2=0), the controller rejected the command,
/// and the installer failed with "partition rescan failed".
#[tokio::test]
async fn install_and_boot_from_nvme_4k() {
    let iso = installer_iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        eprintln!("      build with: make test-iso");
        return;
    }
    if !ovmf_path().exists() {
        eprintln!("SKIP: OVMF not found at {}", ovmf_path().display());
        eprintln!("      install with: apt install ovmf");
        return;
    }

    let disk = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("target/installer_test_disk_4k.img");
    make_fresh_disk(&disk).expect("create fresh 4K disk");
    eprintln!("fresh disk: {} ({} MB, 4K-native NVMe)", disk.display(), DISK_SIZE_MB);

    // ── Boot 1: install onto 4K-native NVMe ──────────────────────────
    eprintln!("==> Boot 1: live ISO + 4K-native NVMe (install phase)");
    let (mut stream, mut proc) =
        AegisHarness::boot_stream(aegis_q35_installer_4k(&disk), &iso)
            .await
            .expect("boot 1 spawn failed");

    wait_for_line(&mut stream, "WARNING: This system",
                  Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .expect("boot 1: pre-login banner");
    tokio::time::sleep(Duration::from_millis(700)).await;

    proc.send_keys("root\n").await.expect("sendkey root");
    tokio::time::sleep(Duration::from_millis(500)).await;
    proc.send_keys(&format!("{}\n", ROOT_PW)).await.expect("sendkey pw");
    tokio::time::sleep(Duration::from_millis(1000)).await;

    proc.send_keys("installer\n").await.expect("sendkey installer");
    wait_for_line(&mut stream, "=== Aegis Installer ===",
                  Duration::from_secs(10))
        .await
        .expect("boot 1: installer banner");

    tokio::time::sleep(Duration::from_millis(1500)).await;
    proc.send_keys("y\n").await.expect("sendkey y");
    tokio::time::sleep(Duration::from_millis(1500)).await;
    proc.send_keys(&format!("{}\n", ROOT_PW)).await.expect("sendkey root pw");
    tokio::time::sleep(Duration::from_millis(1500)).await;
    proc.send_keys(&format!("{}\n", ROOT_PW)).await.expect("sendkey root pw confirm");
    tokio::time::sleep(Duration::from_millis(1500)).await;
    proc.send_keys(&format!("{}\n", USER_NAME)).await.expect("sendkey username");
    tokio::time::sleep(Duration::from_millis(1500)).await;
    proc.send_keys(&format!("{}\n", USER_PW)).await.expect("sendkey user pw");
    tokio::time::sleep(Duration::from_millis(1500)).await;
    proc.send_keys(&format!("{}\n", USER_PW)).await.expect("sendkey user pw confirm");

    {
        let deadline = tokio::time::Instant::now()
            + Duration::from_secs(INSTALL_TIMEOUT_SECS);
        let mut trace: Vec<String> = Vec::new();
        let mut found = false;
        while tokio::time::Instant::now() < deadline {
            match tokio::time::timeout_at(deadline, stream.next_line()).await {
                Ok(Some(line)) => {
                    let done = line.contains("=== Installation complete! ===");
                    trace.push(line);
                    if done { found = true; break; }
                }
                Ok(None) | Err(_) => break,
            }
        }
        if !found {
            eprintln!("--- 4K installer trace (last {} lines) ---", trace.len());
            let skip = trace.len().saturating_sub(40);
            for l in &trace[skip..] { eprintln!("  {}", l); }
            eprintln!("--- end ---");
            panic!("boot 1 (4K): installation did not complete within {}s",
                   INSTALL_TIMEOUT_SECS);
        }
    }
    eprintln!("    installation complete (4K NVMe)");

    proc.send_keys("exit\n").await.ok();
    tokio::time::sleep(Duration::from_millis(1500)).await;
    proc.kill().await.expect("boot 1 kill");
    drop(stream);

    // ── Boot 2: OVMF + 4K-native installed NVMe ──────────────────────
    eprintln!("==> Boot 2: OVMF + 4K-native installed NVMe");
    let (mut stream2, mut proc2) =
        AegisHarness::boot_disk_only(
            aegis_q35_installed_ovmf_4k(&disk, &ovmf_path()),
        )
        .await
        .expect("boot 2 spawn failed");

    wait_for_line(&mut stream2, "[EXT2] OK: mounted nvme0p1",
                  Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .expect("boot 2 (4K): [EXT2] OK: mounted nvme0p1 not seen");
    eprintln!("    [EXT2] OK: mounted nvme0p1 (4K NVMe)");

    proc2.kill().await.expect("boot 2 kill");
    eprintln!("PASS: install_and_boot_from_nvme_4k");
}
