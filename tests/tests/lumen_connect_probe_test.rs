// End-to-end regression test for the lumen_connect AF_UNIX handshake.
// Boots graphical, logs in via Bastion (HMP keys), then waits for
// /bin/lumen-probe (vigil oneshot) to call lumen_connect against
// Lumen's listener and emit [PROBE] markers on the serial console.
//
// The probe binary lives at user/bin/lumen-probe/ and is launched
// automatically by vigil's `lumen-probe` graphical-only oneshot
// service. Probe output goes to /dev/console so it's visible in
// the test harness's serial stream regardless of stdio inheritance.

use aegis_tests::{iso, wait_for_line, AegisHarness};
use std::time::Duration;
use vortex::core::config::QemuOpts;

fn graphical_opts() -> QemuOpts {
    QemuOpts {
        machine: "q35".into(),
        display: "vnc=127.0.0.1:21".into(),
        // No usb-kbd: HMP sendkey routes to PS/2 (the only kbd on
        // q35+LPC), which is what Lumen reads via /dev/kbd.
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

#[tokio::test]
async fn lumen_connect_handshake_via_probe() {
    let iso = iso();
    if !iso.exists() { eprintln!("SKIP: {} not found", iso.display()); return; }

    let (mut stream, mut proc) = AegisHarness::boot_stream(graphical_opts(), &iso)
        .await.expect("boot");

    wait_for_line(&mut stream, "[BASTION] greeter ready",
                  Duration::from_secs(60))
        .await.expect("bastion greeter");
    tokio::time::sleep(Duration::from_millis(500)).await;
    proc.send_keys("root\tforevervigilant\n").await.unwrap();

    // After Bastion auths, vigil starts the graphical-mode services
    // including /bin/lumen-probe. The probe waits for /run/lumen.sock
    // to appear (Lumen creates it on startup), then exercises
    // lumen_connect + lumen_window_create.

    let deadline = tokio::time::Instant::now() + Duration::from_secs(60);
    let mut all: Vec<String> = Vec::new();
    let mut probe_started = false;
    let mut probe_outcome: Option<String> = None;

    loop {
        match tokio::time::timeout_at(deadline, stream.next_line()).await {
            Ok(Some(line)) => {
                eprintln!("LINE: {line}");
                all.push(line.clone());
                if line.contains("[PROBE] starting") {
                    probe_started = true;
                }
                if line.contains("[PROBE] PASS") {
                    probe_outcome = Some("PASS".into());
                    break;
                }
                if line.contains("[PROBE] FAIL") {
                    probe_outcome = Some(line.clone());
                    // Keep collecting briefly for any client diag lines
                    tokio::time::sleep(Duration::from_secs(1)).await;
                    break;
                }
            }
            _ => break,
        }
    }

    let _ = proc.kill().await;

    if !probe_started {
        let probe_lines: Vec<&String> = all.iter().filter(|l|
            l.contains("PROBE") || l.contains("LUMEN-SRV") ||
            l.contains("vigil") || l.contains("lumen-probe")).collect();
        eprintln!("--- relevant lines ---");
        for l in &probe_lines { eprintln!("  {l}"); }
        panic!("probe never started — vigil-launched lumen-probe missing");
    }

    match probe_outcome {
        Some(s) if s == "PASS" => {
            eprintln!("✓ lumen_connect handshake works end-to-end");
        }
        Some(err) => {
            let diag: Vec<&String> = all.iter().filter(|l|
                l.contains("[LUMEN-CLI]") || l.contains("[LUMEN-SRV]") ||
                l.contains("[PROBE]")).collect();
            eprintln!("--- diagnostics ---");
            for l in &diag { eprintln!("  {l}"); }
            panic!("probe failed: {err}");
        }
        None => panic!("probe started but produced no PASS/FAIL marker"),
    }
}
