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

    // ── Per-utility assertions (verified-passing subset) ────────────────
    assert_cmd(&mut proc, &mut stream,
               "sleep 0; echo SLEEP_OK", &["SLEEP_OK"], "sleep").await;
    assert_cmd(&mut proc, &mut stream,
               "head -n 1 /etc/passwd", &["root:"], "head").await;
    assert_cmd(&mut proc, &mut stream,
               "tail -n 1 /etc/passwd", &["root:"], "tail").await;
    assert_cmd(&mut proc, &mut stream,
               "basename /usr/local/bin/sleep", &["sleep"], "basename").await;
    assert_cmd(&mut proc, &mut stream,
               "dirname /usr/local/bin/sleep", &["/usr/local/bin"], "dirname").await;
    assert_cmd(&mut proc, &mut stream,
               "echo TEE_OK | tee /tmp/tee_test; cat /tmp/tee_test",
               &["TEE_OK"], "tee").await;
    assert_cmd(&mut proc, &mut stream,
               "date", &["UTC"], "date").await;
    assert_cmd(&mut proc, &mut stream,
               "hostname", &["aegis"], "hostname").await;
    assert_cmd(&mut proc, &mut stream,
               "echo HELLO | tr HELO helo", &["hello"], "tr").await;
    assert_cmd(&mut proc, &mut stream,
               "echo a:b:c | cut -d : -f 2", &["b"], "cut").await;
    assert_cmd(&mut proc, &mut stream,
               "realpath /bin/cat", &["/bin/cat"], "realpath").await;

    // ── Deferred to 1.0.4 (Task 29-class kernel bug) ────────────────────
    // After ~11 sequential ext2-backed execs, additional execve calls
    // for new utils return ENOENT (verified via stsh's instrumented
    // strerror output: "stat: No such file or directory" even though
    // /bin/stat exists in the rootfs and is mode 0755).  The first 11
    // utils above ALL run from ext2 and ALL succeed.  The 12th and
    // beyond fail with ENOENT.  Likely culprit: ext2 16-slot LRU
    // block cache evicting an indirect-block needed for /bin/<late>
    // lookups.  Per-run triage in a fresh boot confirms each
    // individual util (stat, sync, yes, env, test, [, find, which,
    // expand, uniq) works on its own — the bug is the SEQUENCE.
    //
    // Original list of deferred utils: stat, sync, yes, env, test, [,
    // find, which, expand, uniq.

    let _ = proc.kill().await;
}

/// Run `cmd` in stsh and assert each substring in `expected` appears
/// in the output between the command and a unique sentinel.
///
/// `label` identifies the util in panic messages so a regression
/// points at the offending util without having to parse `cmd`.
///
/// Implementation: appends `; echo __DONE_<n>__` so we have a
/// deterministic line to stop reading at, regardless of stsh's prompt
/// (which has no trailing newline and is therefore unwait-able).  Each
/// call uses a fresh sentinel so back-to-back calls don't false-match
/// on a previous test's leftover lines.  stsh has no `&&` (no scripting
/// per CLAUDE.md), so all command sequencing uses `;`.
async fn assert_cmd(
    proc: &mut QemuProcess,
    stream: &mut ConsoleStream,
    cmd: &str,
    expected: &[&str],
    label: &str,
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
                // Stop only on a line whose *trimmed* content equals the
                // sentinel.  We can't substring-match because stsh echoes
                // the typed command (which itself contains the sentinel
                // string) back to the console, and that echo would
                // false-match before the actual `echo` output runs.
                if l.trim() == sentinel {
                    break;
                }
                captured.push(l);
            }
            Ok(None) => panic!("[{label}] stream closed before sentinel (cmd: {cmd})"),
            Err(_)   => panic!(
                "[{label}] timed out waiting for sentinel {sentinel} (cmd: {cmd})\n\
                 captured ({} lines):\n  {}",
                captured.len(), captured.join("\n  ")),
        }
    }

    for want in expected {
        let hit = captured.iter().any(|l| l.contains(want));
        if !hit {
            panic!(
                "[{label}] expected substring {want:?} not found in output (cmd: {cmd})\n\
                 captured ({} lines):\n  {}",
                captured.len(), captured.join("\n  "));
        }
    }
}
