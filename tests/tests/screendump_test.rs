// End-to-end screendump test: boots Aegis graphically, waits for the
// Bastion greeter to render, and captures the framebuffer via QEMU's
// monitor socket.
//
// Run: AEGIS_ISO=build/aegis.iso cargo test --manifest-path tests/Cargo.toml screendump -- --nocapture
//
// Machine config mirrors `make run-fb`: q35 + virtio-vga. -machine pc
// with -vga std does NOT give GRUB a 32bpp VBE framebuffer that the
// kernel can accept, so sys_fb_map returns -1 and Bastion never renders.
// virtio-vga exposes a Bochs-compatible VBE interface that works.

use aegis_tests::{iso, wait_for_line, AegisHarness};
use std::time::Duration;
use vortex::core::config::QemuOpts;

fn graphical_opts() -> QemuOpts {
    QemuOpts {
        machine: "q35".into(),
        // VNC backend renders the VGA surface headlessly; -display none
        // skips pixel rendering entirely and produces all-black screendumps.
        display: "vnc=127.0.0.1:15".into(),
        devices: vec![
            "virtio-vga".into(),
            "qemu-xhci,id=xhci".into(),
            "usb-kbd,bus=xhci.0".into(),
        ],
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
async fn screendump_on_bastion_greeter() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        return;
    }

    let (mut stream, mut proc) = AegisHarness::boot_stream(graphical_opts(), &iso)
        .await
        .expect("QEMU failed to start");

    // [BASTION] greeter ready fires from draw_form() after the first
    // successful blit_to_fb(), guaranteeing the login form is on screen.
    wait_for_line(&mut stream, "[BASTION] greeter ready", Duration::from_secs(30))
        .await
        .expect("[BASTION] greeter ready never fired within 30s");

    // One extra frame to let anything still painting settle.
    tokio::time::sleep(Duration::from_millis(200)).await;

    let out = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("screenshots/bastion-greeter.ppm");
    let _ = std::fs::remove_file(&out);
    proc.screendump(&out).await.expect("screendump failed");

    // QEMU screendump completes before the monitor reply, but the file
    // write races the (qemu) prompt; a short settle keeps proc.kill()
    // from racing the tail end of the fwrite.
    tokio::time::sleep(Duration::from_millis(200)).await;
    proc.kill().await.unwrap();

    assert!(out.exists(), "screenshot file not created at {}", out.display());
    let sz = out.metadata().unwrap().len();
    // PPM header (~15 bytes) + 640*480*3 pixels = 921,615 bytes minimum
    // for a 640x480 capture. Anything much smaller is a truncated write.
    assert!(sz >= 900_000, "screenshot truncated: only {} bytes", sz);
    eprintln!("screenshot: {} ({} bytes)", out.display(), sz);
}
