// End-to-end GUI installer regression tests.
//
// gui-installer is a Lumen external-window-protocol client (Phase 47).
// It needs a running compositor before it can connect to /run/lumen.sock.
// We use the graphical ISO so Bastion auto-starts Lumen, sign in via
// Bastion's login form, then ask Lumen to spawn gui-installer through
// LUMEN_OP_INVOKE — triggered from the host by sending Ctrl+Alt+I via
// QEMU's HMP `sendkey`, which Lumen handles in main.c.
//
// Boot 1 (full install):
//   1. Graphical ISO + empty NVMe
//   2. Wait for [BASTION] greeter ready, send root + Tab + pw + Enter
//   3. Wait for [LUMEN] ready
//   4. HMP sendkey ctrl-alt-i → Lumen invokes /bin/gui-installer
//   5. Walk the wizard via Enter/Tab/Escape (markers: [INSTALLER] screen=N)
//
// Boot 2 (back button):
//   1. Same as boot 1 through screen=1
//   2. Press Enter → screen=2
//   3. Press Escape → screen=1
//
// Run: AEGIS_ISO=build/aegis.iso cargo test --manifest-path tests/Cargo.toml \
//        --test gui_installer_test -- --nocapture

use aegis_tests::{
    aegis_q35_graphical_mouse, aegis_q35_gui_installer,
    aegis_q35_installed_ovmf, wait_for_line, AegisHarness, QemuProcess,
};
use vortex::config::QemuOpts;
use std::path::PathBuf;
use std::process::Command;
use std::time::Duration;
use tokio::io::AsyncWriteExt;

const DISK_SIZE_MB: u64 = 128;
const BOOT_TIMEOUT_SECS: u64 = 240;
const INSTALL_TIMEOUT_SECS: u64 = 240;

const ROOT_PW: &str = "forevervigilant";
const USER_NAME: &str = "alice";
const USER_PW: &str = "alicepass";

/// Graphical ISO — Bastion auto-starts Lumen.
fn graphical_iso() -> PathBuf {
    let val = std::env::var("AEGIS_ISO")
        .unwrap_or_else(|_| "build/aegis.iso".into());
    PathBuf::from(val)
}

fn disk_path(name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join(format!("target/{}.img", name))
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

/// Send a raw HMP command to QEMU's monitor socket. Used for key chords
/// (`sendkey ctrl-alt-i`) that aren't single-character keystrokes.
async fn hmp_command(proc: &QemuProcess, cmd: &str) -> Result<(), String> {
    let path = proc.monitor_socket_path()
        .ok_or("monitor socket not available")?;
    let mut stream = tokio::net::UnixStream::connect(path)
        .await
        .map_err(|e| format!("connect monitor: {}", e))?;
    let line = format!("{}\n", cmd);
    stream.write_all(line.as_bytes())
        .await
        .map_err(|e| format!("write monitor: {}", e))?;
    Ok(())
}

/// Bastion login → Lumen ready → invoke gui-installer.
/// Returns once `[LUMEN] installer ready` has been seen. `opts` is the
/// QEMU preset to boot — install tests pass a NVMe-backed preset, the
/// back-button test passes a no-disk preset to keep boot fast.
async fn boot_to_installer(
    opts: QemuOpts,
    iso: &std::path::Path,
) -> (aegis_tests::ConsoleStream, QemuProcess) {
    let (mut stream, mut proc) =
        AegisHarness::boot_stream(opts, iso)
            .await
            .expect("QEMU spawn failed");

    wait_for_line(&mut stream, "[BASTION] greeter ready",
                  Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .expect("[BASTION] greeter ready");
    tokio::time::sleep(Duration::from_millis(300)).await;

    proc.send_keys(&format!("root\t{}\n", ROOT_PW))
        .await
        .expect("Bastion login");

    // Lumen startup time varies a lot under TCG (5s warm cache, 30s+
    // cold). Give it a generous window so cold runs don't flake.
    wait_for_line(&mut stream, "[LUMEN] ready",
                  Duration::from_secs(60))
        .await
        .expect("[LUMEN] ready");
    tokio::time::sleep(Duration::from_millis(300)).await;

    hmp_command(&proc, "sendkey ctrl-alt-i")
        .await
        .expect("send Ctrl+Alt+I");

    wait_for_line(&mut stream, "[LUMEN] installer ready",
                  Duration::from_secs(20))
        .await
        .expect("[LUMEN] installer ready");
    wait_for_line(&mut stream, "[INSTALLER] screen=1",
                  Duration::from_secs(5))
        .await
        .expect("[INSTALLER] screen=1");

    (stream, proc)
}

#[tokio::test]
async fn gui_install_and_boot_from_nvme() {
    let iso = graphical_iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        eprintln!("      build with: make iso");
        return;
    }
    if !ovmf_path().exists() {
        eprintln!("SKIP: OVMF not found at {}", ovmf_path().display());
        return;
    }

    let disk = disk_path("gui_installer_test_disk");
    make_fresh_disk(&disk).expect("create fresh disk");
    eprintln!("fresh disk: {} ({} MB)", disk.display(), DISK_SIZE_MB);

    // ── Boot 1: GUI installer ────────────────────────────────────────
    eprintln!("==> Boot 1: graphical ISO + empty NVMe (GUI install)");
    let (mut stream, mut proc) =
        boot_to_installer(aegis_q35_gui_installer(&disk), &iso).await;

    // Screen 1 → 2 (Welcome → Disk)
    tokio::time::sleep(Duration::from_millis(300)).await;
    proc.send_keys("\n").await.expect("welcome→disk");
    wait_for_line(&mut stream, "[INSTALLER] screen=2",
                  Duration::from_secs(5))
        .await
        .expect("screen=2");

    // Screen 2 → 3 (Disk → User)
    tokio::time::sleep(Duration::from_millis(300)).await;
    proc.send_keys("\n").await.expect("disk→user");
    wait_for_line(&mut stream, "[INSTALLER] screen=3",
                  Duration::from_secs(5))
        .await
        .expect("screen=3");

    // Screen 3 → 4 (User form → Confirm)
    tokio::time::sleep(Duration::from_millis(300)).await;
    proc.send_keys(&format!("{}\t{}\t{}\t{}\t{}\t\n",
                            ROOT_PW, ROOT_PW,
                            USER_NAME,
                            USER_PW, USER_PW))
        .await
        .expect("user form");
    wait_for_line(&mut stream, "[INSTALLER] screen=4",
                  Duration::from_secs(10))
        .await
        .expect("screen=4");

    // Screen 4 → 5 (Confirm → Progress)
    tokio::time::sleep(Duration::from_millis(300)).await;
    proc.send_keys("\n").await.expect("confirm→install");
    wait_for_line(&mut stream, "[INSTALLER] screen=5",
                  Duration::from_secs(5))
        .await
        .expect("screen=5");

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
            eprintln!("--- gui-installer trace (last {} lines) ---", trace.len());
            let skip = trace.len().saturating_sub(40);
            for l in &trace[skip..] {
                eprintln!("  {}", l);
            }
            eprintln!("--- end ---");
            panic!("install did not complete within {}s", INSTALL_TIMEOUT_SECS);
        }
    }
    eprintln!("    installation complete");

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
        .expect("boot 2: ext2 mount");
    eprintln!("    [EXT2] OK: mounted nvme0p1");

    proc2.kill().await.expect("boot 2 kill");
    eprintln!("PASS: gui_install_and_boot_from_nvme");
}

/// Back-button regression: screen=1 → Enter → screen=2 → Escape → screen=1.
#[tokio::test]
async fn gui_installer_back_button() {
    let iso = graphical_iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        return;
    }

    // No NVMe needed — back-button test only walks screens.
    let (mut stream, mut proc) =
        boot_to_installer(aegis_q35_graphical_mouse(), &iso).await;

    tokio::time::sleep(Duration::from_millis(300)).await;
    proc.send_keys("\n").await.expect("Enter to advance");
    wait_for_line(&mut stream, "[INSTALLER] screen=2",
                  Duration::from_secs(5))
        .await
        .expect("screen=2 after Enter");

    tokio::time::sleep(Duration::from_millis(300)).await;
    proc.send_keys("\x1b").await.expect("Escape to go back");
    wait_for_line(&mut stream, "[INSTALLER] screen=1",
                  Duration::from_secs(5))
        .await
        .expect("screen=1 after Escape");

    proc.kill().await.expect("kill");
    eprintln!("PASS: gui_installer_back_button");
}
