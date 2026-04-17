// coreutils_test.rs — scaffold for coreutils-1.0.3 per-utility tests.
//
// Boots the text-mode aegis-test.iso, logs in as root, and waits for
// stsh to emit `[STSH] ready` (a one-shot marker printed once the REPL
// is about to draw its first prompt). After that, future tasks add
// per-utility assertions (head, tail, env, ...) using `assert_cmd`.
//
// Why text-mode (aegis-test.iso) and not aegis-installer-test.iso:
// the installer-test ISO boots into Bastion → Lumen, where stsh only
// runs inside an opened terminal window. Text-mode puts stsh directly
// on the console, which is what coreutils tests need to drive.
//
// Run: AEGIS_TEST_ISO=build/aegis-test.iso \
//      cargo test --manifest-path tests/Cargo.toml \
//                 --test coreutils_test -- --nocapture

use aegis_tests::{
    aegis_q35_installer, wait_for_line, AegisHarness, ConsoleStream, QemuProcess,
};
use std::path::PathBuf;
use std::process::Command;
use std::time::Duration;

const DISK_SIZE_MB: u64 = 32;
const BOOT_TIMEOUT_SECS: u64 = 120;
const ROOT_PW: &str = "forevervigilant";
const STSH_READY_MARKER: &str = "[STSH] ready";

fn test_iso() -> PathBuf {
    let val = std::env::var("AEGIS_TEST_ISO")
        .unwrap_or_else(|_| "build/aegis-test.iso".into());
    PathBuf::from(val)
}

fn disk_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("target/coreutils_disk.img")
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
            format!("truncate failed with status {status}"),
        ));
    }
    Ok(())
}

#[tokio::test]
async fn coreutils_scaffold_boots_to_stsh_ready() {
    let iso = test_iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found (run `make test-iso`)", iso.display());
        return;
    }

    let disk = disk_path();
    make_fresh_disk(&disk).expect("create fresh disk");

    let (mut stream, mut proc) =
        AegisHarness::boot_stream(aegis_q35_installer(&disk), &iso)
            .await
            .expect("boot spawn failed");

    // Anchor on the pre-login banner to know /bin/login has drawn the prompt.
    wait_for_line(&mut stream, "WARNING: This system",
                  Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .expect("pre-login banner never appeared");

    // Let /bin/login draw its `login:` prompt before we type.
    tokio::time::sleep(Duration::from_millis(700)).await;
    proc.send_keys("root\n").await.expect("send username");
    tokio::time::sleep(Duration::from_millis(500)).await;
    proc.send_keys(&format!("{ROOT_PW}\n")).await.expect("send password");

    // Wait for stsh to come up. The marker is emitted once on REPL entry.
    wait_for_line(&mut stream, STSH_READY_MARKER,
                  Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .unwrap_or_else(|e| panic!(
            "stsh ready marker '{STSH_READY_MARKER}' never appeared: {e:?}"));

    // ── Per-utility assertions ─────────────────────────────────────────
    // sleep — exits 0 immediately when given 0 seconds.
    assert_cmd(&mut proc, &mut stream,
               "sleep 0; echo SLEEP_OK", &["SLEEP_OK"]).await;

    let _ = proc.kill().await;
}

/// Run `cmd` in stsh and assert each substring in `expected` appears
/// in the output between the command and its sentinel.
///
/// Implementation: appends `; echo __DONE_<n>__` so we have a
/// deterministic line to stop reading at, regardless of stsh's prompt
/// (which has no trailing newline and is therefore unwait-able).  Each
/// call uses a fresh sentinel so back-to-back calls don't false-match
/// on a previous test's leftover lines.
async fn assert_cmd(
    proc: &mut QemuProcess,
    stream: &mut ConsoleStream,
    cmd: &str,
    expected: &[&str],
) {
    use std::sync::atomic::{AtomicUsize, Ordering};
    static SEQ: AtomicUsize = AtomicUsize::new(0);
    let n = SEQ.fetch_add(1, Ordering::Relaxed);
    let sentinel = format!("__DONE_{n}__");

    let line = format!("{cmd}; echo {sentinel}\n");
    proc.send_keys(&line).await.expect("send_keys");

    let deadline = tokio::time::Instant::now() + Duration::from_secs(15);
    let mut captured: Vec<String> = Vec::new();
    loop {
        match tokio::time::timeout_at(deadline, stream.next_line()).await {
            Ok(Some(l)) => {
                if l.contains(&sentinel) {
                    break;
                }
                captured.push(l);
            }
            Ok(None) => panic!("[{cmd}] stream closed before sentinel"),
            Err(_)   => panic!(
                "[{cmd}] timed out waiting for sentinel {sentinel}\n\
                 captured ({} lines):\n  {}",
                captured.len(), captured.join("\n  ")),
        }
    }

    for want in expected {
        let hit = captured.iter().any(|l| l.contains(want));
        if !hit {
            panic!(
                "[{cmd}] expected substring {want:?} not found in output\n\
                 captured ({} lines):\n  {}",
                captured.len(), captured.join("\n  "));
        }
    }
}
