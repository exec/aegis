// End-to-end login flow test: boots Aegis, asserts the Bastion greeter
// matches the reference screenshot, types credentials, waits for Lumen
// to finish fading in, and captures the desktop.
//
// First run: generates `screenshots/lumen-desktop.ppm` as the reference.
// Subsequent runs: asserts the current capture matches that reference.
// Force a refresh of both references with AEGIS_UPDATE_SCREENSHOTS=1.
//
// Run: AEGIS_ISO=build/aegis.iso cargo test --manifest-path tests/Cargo.toml login_to_lumen_desktop -- --nocapture

use aegis_tests::{assert_ppm_matches, iso, wait_for_line, AegisHarness};
use std::time::Duration;
use vortex::core::config::QemuOpts;

fn graphical_opts() -> QemuOpts {
    QemuOpts {
        machine: "q35".into(),
        display: "vnc=127.0.0.1:15".into(),
        // NO usb-kbd: QEMU's HMP `sendkey` routes to the first keyboard
        // device; when a usb-kbd is present it gets the keys instead of
        // the i8042 PS/2 keyboard that Bastion reads from via /dev/kbd.
        // On q35 + LPC, PS/2 is auto-provided by the chipset.
        devices: vec!["virtio-vga".into()],
        drives: vec![],
        extra_args: vec![
            "-cpu".into(), "Broadwell".into(),
            "-no-reboot".into(),
        ],
        serial_capture: true,
        monitor_socket: true,
    }
}

fn update_mode() -> bool {
    std::env::var("AEGIS_UPDATE_SCREENSHOTS").is_ok()
}

#[tokio::test]
async fn login_to_lumen_desktop() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        return;
    }

    let manifest_dir = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let bastion_ref = manifest_dir.join("screenshots/bastion-greeter.ppm");
    let lumen_ref = manifest_dir.join("screenshots/lumen-desktop.ppm");
    let bastion_actual = manifest_dir.join("screenshots/tmp-bastion-actual.ppm");
    let lumen_actual = manifest_dir.join("screenshots/tmp-lumen-actual.ppm");
    let _ = std::fs::remove_file(&bastion_actual);
    let _ = std::fs::remove_file(&lumen_actual);

    let (mut stream, mut proc) = AegisHarness::boot_stream(graphical_opts(), &iso)
        .await
        .expect("QEMU failed to start");

    // ── 1. Wait for Bastion greeter ─────────────────────────────────
    wait_for_line(&mut stream, "[BASTION] greeter ready", Duration::from_secs(30))
        .await
        .expect("[BASTION] greeter ready never fired within 30s");
    tokio::time::sleep(Duration::from_millis(200)).await;

    // ── 2. Capture + compare Bastion greeter ────────────────────────
    proc.screendump(&bastion_actual)
        .await
        .expect("bastion screendump failed");
    tokio::time::sleep(Duration::from_millis(200)).await;

    if update_mode() || !bastion_ref.exists() {
        std::fs::rename(&bastion_actual, &bastion_ref).unwrap();
        eprintln!("updated reference: {}", bastion_ref.display());
    } else {
        assert_ppm_matches(&bastion_actual, &bastion_ref);
        std::fs::remove_file(&bastion_actual).ok();
        eprintln!("bastion greeter matches reference ✓");
    }

    // ── 3. Type credentials: root<Tab>forevervigilant<Enter> ────────
    proc.send_keys("root\tforevervigilant\n")
        .await
        .expect("send_keys failed");
    eprintln!("sent: root<Tab>forevervigilant<Enter>");

    // ── 4. Wait for Lumen to finish fading in ───────────────────────
    wait_for_line(&mut stream, "[LUMEN] ready", Duration::from_secs(20))
        .await
        .expect("[LUMEN] ready never fired within 20s");

    // Short settle before clock tick updates — Lumen's topbar clock
    // refreshes roughly once per second, so stay under that window.
    tokio::time::sleep(Duration::from_millis(300)).await;

    // ── 5. Capture Lumen desktop ────────────────────────────────────
    proc.screendump(&lumen_actual)
        .await
        .expect("lumen screendump failed");
    tokio::time::sleep(Duration::from_millis(200)).await;

    if update_mode() || !lumen_ref.exists() {
        std::fs::rename(&lumen_actual, &lumen_ref).unwrap();
        eprintln!(
            "captured lumen desktop reference: {}",
            lumen_ref.display()
        );
    } else {
        assert_ppm_matches(&lumen_actual, &lumen_ref);
        std::fs::remove_file(&lumen_actual).ok();
        eprintln!("lumen desktop matches reference ✓");
    }

    proc.kill().await.unwrap();
}
