// Boot graphical, log into Lumen, dump all serial output and look for
// [LUMEN-SRV] listening on /run/lumen.sock — confirming the AF_UNIX
// listener was created successfully. If it's missing, lumen_server_init
// failed silently.

use aegis_tests::{iso, wait_for_line, AegisHarness};
use std::time::Duration;
use vortex::core::config::QemuOpts;

fn graphical_opts() -> QemuOpts {
    QemuOpts {
        machine: "q35".into(),
        display: "vnc=127.0.0.1:18".into(),
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
async fn lumen_server_init_logs() {
    let iso = iso();
    if !iso.exists() { eprintln!("SKIP: {} not found", iso.display()); return; }

    let (mut stream, mut proc) = AegisHarness::boot_stream(graphical_opts(), &iso)
        .await
        .expect("boot");

    wait_for_line(&mut stream, "[BASTION] greeter ready", Duration::from_secs(60))
        .await
        .expect("bastion");

    tokio::time::sleep(Duration::from_millis(500)).await;
    proc.send_keys("root\tforevervigilant\n").await.expect("creds");

    // Dump ALL serial output for 25s after login.
    let deadline = tokio::time::Instant::now() + Duration::from_secs(25);
    let mut all: Vec<String> = Vec::new();
    let mut saw_listening = false;
    loop {
        match tokio::time::timeout_at(deadline, stream.next_line()).await {
            Ok(Some(line)) => {
                eprintln!("LINE: {line}");
                all.push(line.clone());
                if line.contains("listening on /run/lumen.sock") {
                    saw_listening = true;
                }
            }
            _ => break,
        }
    }

    let _ = proc.kill().await;

    if !saw_listening {
        panic!("[LUMEN-SRV] listening line not found — server init failed");
    }
}
