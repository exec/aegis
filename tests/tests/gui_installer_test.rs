// End-to-end GUI installer regression tests.
//
// gui-installer is a Lumen external-window-protocol client (Phase 47).
// It needs a running compositor with admin caps before it can do its
// job, so we boot `aegis-installer-test.iso` — a graphical ISO whose
// kernel cmdline contains `bastion_autologin=root`. Bastion sees the
// flag, skips the greeter form, and authenticates as root with the
// hardcoded test password. This avoids the input-driven login race
// that made earlier versions of this test flake on cold boot.
//
// Once Lumen is up, the host fires Ctrl+Alt+I via QEMU's HMP `sendkey`
// (lumen/main.c maps that chord to LUMEN_OP_INVOKE "gui-installer"),
// then drives the wizard via Enter / Tab / Escape on serial markers.
//
// Run: make installer-test-iso
//      AEGIS_INSTALLER_TEST_ISO=build/aegis-installer-test.iso \
//        cargo test --manifest-path tests/Cargo.toml \
//        --test gui_installer_test -- --nocapture

use aegis_tests::{
    aegis_q35_graphical_mouse, aegis_q35_gui_installer,
    aegis_q35_gui_installer_4k,
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

/// Installer-test ISO — graphical boot with bastion_autologin=root on
/// the kernel cmdline. Bastion skips the greeter form and authenticates
/// using the hardcoded test password ("forevervigilant"), which lets
/// us avoid the send_keys / Bastion input race that made earlier
/// versions of this test flake on cold boot.
fn installer_test_iso() -> PathBuf {
    let val = std::env::var("AEGIS_INSTALLER_TEST_ISO")
        .unwrap_or_else(|_| "build/aegis-installer-test.iso".into());
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

/// Boot installer-test ISO → Bastion autologin → Lumen → invoke
/// gui-installer. No keyboard interaction during boot — Bastion
/// reads `bastion_autologin=root` from the kernel cmdline. The only
/// host→guest input is the Ctrl+Alt+I chord that asks Lumen to spawn
/// gui-installer.
async fn boot_to_installer(
    opts: QemuOpts,
    iso: &std::path::Path,
) -> (aegis_tests::ConsoleStream, QemuProcess) {
    let (mut stream, mut proc) =
        AegisHarness::boot_stream(opts, iso)
            .await
            .expect("QEMU spawn failed");

    wait_for_line(&mut stream, "[BASTION] autologin OK",
                  Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .expect("[BASTION] autologin OK");

    wait_for_line(&mut stream, "[LUMEN] ready",
                  Duration::from_secs(30))
        .await
        .expect("[LUMEN] ready");
    tokio::time::sleep(Duration::from_millis(500)).await;

    let mut installer_up = false;
    for _ in 0..6 {
        hmp_command(&proc, "sendkey ctrl-alt-i")
            .await
            .expect("ctrl-alt-i");
        if wait_for_line(&mut stream, "[LUMEN] installer ready",
                         Duration::from_secs(10))
            .await
            .is_ok()
        {
            installer_up = true;
            break;
        }
    }
    if !installer_up {
        proc.kill().await.ok();
        panic!("installer never appeared after 6 Ctrl+Alt+I retries");
    }
    wait_for_line(&mut stream, "[INSTALLER] screen=1",
                  Duration::from_secs(5))
        .await
        .expect("[INSTALLER] screen=1");

    (stream, proc)
}

#[tokio::test]
async fn gui_install_and_boot_from_nvme() {
    let iso = installer_test_iso();
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

    // Reproduces the bare-metal symptom: after a fresh install, the
    // first NVMe boot reaches Bastion but keystrokes appear not to
    // reach the input loop. Walk the full path: ext2 mount → Bastion
    // greeter → submit root credentials → expect Lumen to start. If
    // Lumen never starts, dump the last serial lines so we can see
    // where the chain breaks.
    let mut trace: Vec<String> = Vec::new();
    let dump_trace = |trace: &Vec<String>, label: &str| {
        eprintln!("--- post-install serial trace ({}; last {} lines) ---",
                  label, trace.len().min(80));
        let skip = trace.len().saturating_sub(80);
        for l in &trace[skip..] { eprintln!("  {}", l); }
        eprintln!("--- end ---");
    };

    // Drain serial until we see a marker, accumulating into `trace`.
    async fn drain_until(
        stream: &mut aegis_tests::ConsoleStream,
        trace: &mut Vec<String>,
        marker: &str,
        timeout: Duration,
    ) -> Result<(), ()> {
        let deadline = tokio::time::Instant::now() + timeout;
        while tokio::time::Instant::now() < deadline {
            match tokio::time::timeout_at(deadline, stream.next_line()).await {
                Ok(Some(line)) => {
                    let hit = line.contains(marker);
                    trace.push(line);
                    if hit { return Ok(()); }
                }
                _ => break,
            }
        }
        Err(())
    }

    if drain_until(&mut stream2, &mut trace,
                   "[EXT2] OK: mounted nvme0p1",
                   Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await.is_err()
    {
        dump_trace(&trace, "no [EXT2] OK");
        proc2.kill().await.ok();
        panic!("boot 2: ext2 mount never appeared");
    }
    eprintln!("    [EXT2] OK: mounted nvme0p1");

    if drain_until(&mut stream2, &mut trace,
                   "[BASTION] greeter ready",
                   Duration::from_secs(60))
        .await.is_err()
    {
        dump_trace(&trace, "no [BASTION] greeter ready");
        proc2.kill().await.ok();
        panic!("boot 2: Bastion greeter never appeared");
    }
    eprintln!("    [BASTION] greeter ready");

    // Type root credentials. send_keys maps to HMP sendkey per char.
    tokio::time::sleep(Duration::from_millis(300)).await;
    proc2.send_keys(&format!("root\t{}\n", ROOT_PW))
        .await
        .expect("send root credentials");

    // If keystrokes reach the read loop, bastion will auth and spawn
    // Lumen. If they don't, we'll time out here — the smoking gun for
    // the bare-metal symptom.
    if drain_until(&mut stream2, &mut trace,
                   "[LUMEN] ready",
                   Duration::from_secs(30))
        .await.is_err()
    {
        dump_trace(&trace, "no [LUMEN] ready after sending credentials");
        proc2.kill().await.ok();
        panic!("boot 2: Lumen never started — keystrokes likely not reaching Bastion");
    }
    eprintln!("    [LUMEN] ready (keystrokes reached Bastion)");

    proc2.kill().await.expect("boot 2 kill");
    eprintln!("PASS: gui_install_and_boot_from_nvme");
}

/// Re-install on a disk that already contains an Aegis install.
/// User reports rescan fails when the disk has an existing install.
/// This test reproduces that scenario: install once, then run the
/// installer again on the same disk without wiping it.
#[tokio::test]
async fn gui_installer_overwrite_existing() {
    let iso = installer_test_iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        return;
    }

    let disk = disk_path("gui_installer_overwrite_disk");
    make_fresh_disk(&disk).expect("create fresh disk");

    // Use 4K-native NVMe — modern consumer SSDs (Samsung, etc.) report
    // 4K logical block size, and rescan-after-install regressions tend
    // to surface there (sub-block alignment in the 512e wrapper, write
    // ordering through the bounce buffer, etc.).
    let preset = aegis_q35_gui_installer_4k;

    async fn run_install_screens(
        proc: &mut QemuProcess,
        stream: &mut aegis_tests::ConsoleStream,
        label: &str,
    ) -> Result<(), String> {
        // Walk screens 1→2→3→4→5
        tokio::time::sleep(Duration::from_millis(300)).await;
        proc.send_keys("\n").await.map_err(|e| format!("{}: welcome→disk: {}", label, e))?;
        wait_for_line(stream, "[INSTALLER] screen=2", Duration::from_secs(5))
            .await.map_err(|_| format!("{}: screen=2", label))?;
        tokio::time::sleep(Duration::from_millis(300)).await;
        proc.send_keys("\n").await.map_err(|e| format!("{}: disk→user: {}", label, e))?;
        wait_for_line(stream, "[INSTALLER] screen=3", Duration::from_secs(5))
            .await.map_err(|_| format!("{}: screen=3", label))?;
        tokio::time::sleep(Duration::from_millis(300)).await;
        proc.send_keys(&format!("{}\t{}\t{}\t{}\t{}\t\n",
                                ROOT_PW, ROOT_PW,
                                USER_NAME, USER_PW, USER_PW))
            .await.map_err(|e| format!("{}: user form: {}", label, e))?;
        wait_for_line(stream, "[INSTALLER] screen=4", Duration::from_secs(10))
            .await.map_err(|_| format!("{}: screen=4", label))?;
        tokio::time::sleep(Duration::from_millis(300)).await;
        proc.send_keys("\n").await.map_err(|e| format!("{}: confirm→install: {}", label, e))?;
        wait_for_line(stream, "[INSTALLER] screen=5", Duration::from_secs(5))
            .await.map_err(|_| format!("{}: screen=5", label))?;
        Ok(())
    }

    // ── Boot 1: first install on empty NVMe ────────────────────────
    eprintln!("==> Boot 1: install on empty NVMe (must succeed)");
    let (mut stream1, mut proc1) =
        boot_to_installer(preset(&disk), &iso).await;
    run_install_screens(&mut proc1, &mut stream1, "boot1")
        .await.expect("boot1 screens");
    wait_for_line(&mut stream1, "[INSTALLER] done",
                  Duration::from_secs(INSTALL_TIMEOUT_SECS))
        .await.expect("boot1 [INSTALLER] done");
    eprintln!("    boot1 install complete");
    proc1.kill().await.expect("boot1 kill");
    drop(stream1);

    // ── Boot 2: install AGAIN on same disk (the reported failure) ──
    eprintln!("==> Boot 2: install on already-installed NVMe");
    let (mut stream2, mut proc2) =
        boot_to_installer(preset(&disk), &iso).await;
    run_install_screens(&mut proc2, &mut stream2, "boot2")
        .await.expect("boot2 screens");

    // Capture every line from screen=5 until either [INSTALLER] done
    // or [INSTALLER] error=. Whichever fires tells us whether the
    // overwrite path is fixed.
    let mut trace: Vec<String> = Vec::new();
    let deadline = tokio::time::Instant::now()
        + Duration::from_secs(INSTALL_TIMEOUT_SECS);
    let mut outcome: Option<String> = None;
    while tokio::time::Instant::now() < deadline {
        match tokio::time::timeout_at(deadline, stream2.next_line()).await {
            Ok(Some(line)) => {
                let done = line.contains("[INSTALLER] done");
                let err = line.contains("[INSTALLER] error=");
                trace.push(line.clone());
                if done { outcome = Some("done".into()); break; }
                if err { outcome = Some(line); break; }
            }
            _ => break,
        }
    }
    proc2.kill().await.ok();

    eprintln!("--- boot2 install trace (last {} lines) ---", trace.len().min(40));
    let skip = trace.len().saturating_sub(40);
    for l in &trace[skip..] { eprintln!("  {}", l); }
    eprintln!("--- end ---");

    match outcome {
        Some(s) if s == "done" => eprintln!("PASS: re-install succeeded"),
        Some(err) => panic!("re-install failed: {}", err),
        None => panic!("re-install neither completed nor errored within {}s",
                       INSTALL_TIMEOUT_SECS),
    }
}

/// Back-button regression: screen=1 → Enter → screen=2 → Escape → screen=1.
#[tokio::test]
async fn gui_installer_back_button() {
    let iso = installer_test_iso();
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
