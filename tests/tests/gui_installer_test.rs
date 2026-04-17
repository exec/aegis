// End-to-end GUI installer regression test.
//
// Mirrors installer_test.rs but drives the graphical wizard via
// Tab/Enter keyboard navigation instead of the text installer's
// CLI prompts.
//
// Boot 1 flow:
//   1. Live text ISO + empty NVMe
//   2. Log into stsh as root
//   3. Launch /bin/gui-installer
//   4. Wait for [LUMEN] installer ready + [INSTALLER] screen=1
//   5. Press Enter → [INSTALLER] screen=2 (disk selection)
//   6. Press Enter → [INSTALLER] screen=3 (user account)
//   7. Type root pw, Tab, confirm, Tab, username, Tab, user pw, Tab,
//      user pw confirm, Tab (focus=Next), Enter → [INSTALLER] screen=4
//   8. Press Enter → [INSTALLER] screen=5
//   9. Wait for [INSTALLER] done
//
// Boot 2 flow: OVMF + installed NVMe, verify [EXT2] OK: mounted nvme0p1.
//
// Uses aegis-test.iso (boot=text) — same as installer_test — because
// the live graphical ISO boots Bastion which blocks stsh access, and
// the GUI installer opens its own framebuffer independent of Bastion.
//
// Run: AEGIS_INSTALLER_ISO=build/aegis-test.iso cargo test --manifest-path tests/Cargo.toml --test gui_installer_test -- --nocapture

use aegis_tests::{
    aegis_q35_gui_installer, aegis_q35_installed_ovmf, wait_for_line,
    AegisHarness,
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

/// Text-mode ISO — same rationale as installer_test.
fn installer_iso() -> PathBuf {
    let val = std::env::var("AEGIS_INSTALLER_ISO")
        .unwrap_or_else(|_| "build/aegis-test.iso".into());
    PathBuf::from(val)
}

fn disk_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("target/gui_installer_test_disk.img")
}

fn ovmf_path() -> PathBuf {
    PathBuf::from("/usr/share/OVMF/OVMF_CODE_4M.fd")
}

fn make_fresh_disk(path: &std::path::Path) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let _ = std::fs::remove_file(path);
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
async fn gui_install_and_boot_from_nvme() {
    let iso = installer_iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        eprintln!("      build with: make test-iso");
        return;
    }
    if !ovmf_path().exists() {
        eprintln!("SKIP: OVMF not found at {}", ovmf_path().display());
        return;
    }

    let disk = disk_path();
    make_fresh_disk(&disk).expect("create fresh disk");
    eprintln!("fresh disk: {} ({} MB)", disk.display(), DISK_SIZE_MB);

    // ── Boot 1: GUI installer ────────────────────────────────────────
    eprintln!("==> Boot 1: live text ISO + empty NVMe (GUI install)");
    let (mut stream, mut proc) =
        AegisHarness::boot_stream(aegis_q35_gui_installer(&disk), &iso)
            .await
            .expect("boot 1 spawn failed");

    // Pre-login banner (newline-terminated — wait_for_line can match).
    wait_for_line(&mut stream, "WARNING: This system",
                  Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .expect("boot 1: pre-login banner never appeared");
    tokio::time::sleep(Duration::from_millis(700)).await;

    // login: root / forevervigilant.
    proc.send_keys("root\n").await.expect("sendkey root");
    tokio::time::sleep(Duration::from_millis(500)).await;
    proc.send_keys(&format!("{}\n", ROOT_PW))
        .await
        .expect("sendkey login pw");
    tokio::time::sleep(Duration::from_millis(1000)).await;

    // Launch the GUI installer from stsh.
    proc.send_keys("gui-installer\n")
        .await
        .expect("sendkey gui-installer");

    // Wait for the wizard to come up.
    wait_for_line(&mut stream, "[LUMEN] installer ready",
                  Duration::from_secs(20))
        .await
        .expect("boot 1: GUI installer ready");
    wait_for_line(&mut stream, "[INSTALLER] screen=1",
                  Duration::from_secs(5))
        .await
        .expect("boot 1: screen 1 marker");

    // ── Screen 1 → Screen 2 (Welcome → Disk) ──
    tokio::time::sleep(Duration::from_millis(300)).await;
    proc.send_keys("\n").await.expect("sendkey welcome-next");
    wait_for_line(&mut stream, "[INSTALLER] screen=2",
                  Duration::from_secs(5))
        .await
        .expect("boot 1: screen 2 marker");

    // ── Screen 2 → Screen 3 (Disk → User) ──
    // Only one disk (nvme0) is available; Enter confirms.
    tokio::time::sleep(Duration::from_millis(300)).await;
    proc.send_keys("\n").await.expect("sendkey disk-next");
    wait_for_line(&mut stream, "[INSTALLER] screen=3",
                  Duration::from_secs(5))
        .await
        .expect("boot 1: screen 3 marker");

    // ── Screen 3 → Screen 4 (User → Confirm) ──
    // Focus starts at field 0 (root_pw). Each \t advances focus by 1.
    // After 5 tabs we're on Next (focus=5). Final \n submits.
    tokio::time::sleep(Duration::from_millis(300)).await;
    proc.send_keys(&format!("{}\t{}\t{}\t{}\t{}\t\n",
                            ROOT_PW, ROOT_PW,
                            USER_NAME,
                            USER_PW, USER_PW))
        .await
        .expect("sendkey user form");
    wait_for_line(&mut stream, "[INSTALLER] screen=4",
                  Duration::from_secs(10))
        .await
        .expect("boot 1: screen 4 marker");

    // ── Screen 4 → Screen 5 (Confirm → Progress) ──
    tokio::time::sleep(Duration::from_millis(300)).await;
    proc.send_keys("\n").await.expect("sendkey confirm-install");
    wait_for_line(&mut stream, "[INSTALLER] screen=5",
                  Duration::from_secs(5))
        .await
        .expect("boot 1: screen 5 marker");

    // Wait for install completion (with serial trace on timeout).
    {
        let deadline = tokio::time::Instant::now()
            + Duration::from_secs(INSTALL_TIMEOUT_SECS);
        let mut trace: Vec<String> = Vec::new();
        let mut found = false;
        while tokio::time::Instant::now() < deadline {
            match tokio::time::timeout_at(deadline, stream.next_line()).await {
                Ok(Some(line)) => {
                    let done = line.contains("[INSTALLER] done");
                    trace.push(line);
                    if done { found = true; break; }
                }
                Ok(None) | Err(_) => break,
            }
        }
        if !found {
            eprintln!("--- gui-installer serial trace (last {} lines) ---",
                      trace.len());
            let skip = trace.len().saturating_sub(40);
            for l in &trace[skip..] {
                eprintln!("  {}", l);
            }
            eprintln!("--- end ---");
            panic!("boot 1: GUI installation did not complete within {}s",
                   INSTALL_TIMEOUT_SECS);
        }
    }
    eprintln!("    installation complete");

    // Kill rather than pressing Reboot — avoids reboot-path side effects.
    tokio::time::sleep(Duration::from_millis(500)).await;
    proc.kill().await.expect("boot 1 kill");
    drop(stream);

    // ── Boot 2: OVMF + installed NVMe ────────────────────────────────
    eprintln!("==> Boot 2: OVMF + installed NVMe");
    let (mut stream2, mut proc2) =
        AegisHarness::boot_disk_only(
            aegis_q35_installed_ovmf(&disk, &ovmf_path()),
        )
        .await
        .expect("boot 2 spawn failed");

    wait_for_line(&mut stream2, "[EXT2] OK: mounted nvme0p1",
                  Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .expect("boot 2: [EXT2] OK: mounted nvme0p1 not seen");
    eprintln!("    [EXT2] OK: mounted nvme0p1");

    proc2.kill().await.expect("boot 2 kill");
    eprintln!("PASS: gui_install_and_boot_from_nvme");
}

/// Back-button regression test.
///
/// Advances the GUI installer from Welcome (screen=1) to Disk (screen=2)
/// then sends Escape (which maps to handle_back()) and asserts the wizard
/// returns to screen=1.  Catches regressions where the back handler is
/// removed, the Escape binding is dropped, or the screen-1 marker stops
/// being emitted.
///
/// No second boot — this test only exercises wizard navigation.
#[tokio::test]
async fn gui_installer_back_button() {
    let iso = installer_iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        eprintln!("      build with: make test-iso");
        return;
    }

    let disk = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("target/gui_installer_back_test_disk.img");
    make_fresh_disk(&disk).expect("create fresh disk");

    let (mut stream, mut proc) =
        AegisHarness::boot_stream(aegis_q35_gui_installer(&disk), &iso)
            .await
            .expect("boot spawn failed");

    wait_for_line(&mut stream, "WARNING: This system",
                  Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .expect("pre-login banner");
    tokio::time::sleep(Duration::from_millis(700)).await;

    proc.send_keys("root\n").await.expect("sendkey root");
    tokio::time::sleep(Duration::from_millis(500)).await;
    proc.send_keys(&format!("{}\n", ROOT_PW)).await.expect("sendkey pw");
    tokio::time::sleep(Duration::from_millis(1000)).await;

    proc.send_keys("gui-installer\n").await.expect("launch gui-installer");

    wait_for_line(&mut stream, "[LUMEN] installer ready",
                  Duration::from_secs(20))
        .await
        .expect("installer ready");
    wait_for_line(&mut stream, "[INSTALLER] screen=1",
                  Duration::from_secs(5))
        .await
        .expect("screen=1 on launch");

    // Advance to screen 2.
    tokio::time::sleep(Duration::from_millis(300)).await;
    proc.send_keys("\n").await.expect("advance to disk screen");
    wait_for_line(&mut stream, "[INSTALLER] screen=2",
                  Duration::from_secs(5))
        .await
        .expect("screen=2 after Enter");

    // Press Escape — should trigger handle_back() → screen=1.
    tokio::time::sleep(Duration::from_millis(300)).await;
    proc.send_keys("\x1b").await.expect("sendkey Escape");
    wait_for_line(&mut stream, "[INSTALLER] screen=1",
                  Duration::from_secs(5))
        .await
        .expect("screen=1 after Escape (back button regression)");

    proc.kill().await.expect("kill");
    eprintln!("PASS: gui_installer_back_button");
}
