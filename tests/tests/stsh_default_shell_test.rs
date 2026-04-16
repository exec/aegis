// Verify that text-mode login spawns /bin/stsh (not /bin/sh) by default.
//
// /etc/passwd's root entry ends in /bin/stsh. /bin/login execve's that path
// and falls back to /bin/sh on failure. This test boots the text-mode ISO,
// logs in as root, and runs the stsh-only `caps` builtin. If stsh is
// running, `caps` prints a list of capability names. If /bin/sh is running
// instead, it prints "caps: not found" and the test fails.
//
// Requires build/aegis-test.iso (`make test-iso`) — the live graphical
// ISO would land in Bastion, not /bin/login.
//
// Run: AEGIS_INSTALLER_ISO=build/aegis-test.iso \
//      cargo test --manifest-path tests/Cargo.toml --test stsh_default_shell_test -- --nocapture

use aegis_tests::{aegis_q35_installer, wait_for_line, AegisHarness};
use std::path::PathBuf;
use std::process::Command;
use std::time::Duration;

const DISK_SIZE_MB: u64 = 32;
const BOOT_TIMEOUT_SECS: u64 = 120;
const ROOT_PW: &str = "forevervigilant";

fn installer_iso() -> PathBuf {
    let val = std::env::var("AEGIS_INSTALLER_ISO")
        .unwrap_or_else(|_| "build/aegis-test.iso".into());
    PathBuf::from(val)
}

fn disk_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("target/stsh_default_shell_disk.img")
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
async fn login_spawns_stsh() {
    let iso = installer_iso();
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

    // Anchor on the pre-login banner (terminated with newline).
    wait_for_line(&mut stream, "WARNING: This system",
                  Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .expect("pre-login banner never appeared");

    // Let /bin/login draw its `login: ` prompt.
    tokio::time::sleep(Duration::from_millis(700)).await;

    proc.send_keys("root\n").await.expect("send username");
    tokio::time::sleep(Duration::from_millis(500)).await;
    proc.send_keys(&format!("{ROOT_PW}\n"))
        .await
        .expect("send password");
    tokio::time::sleep(Duration::from_millis(1500)).await;

    // After login, the prompt is drawn without a trailing newline so
    // wait_for_line can't see it. Send a stsh-only builtin and look at
    // the output. `caps` prints capability names like "IPC(R)" on stsh,
    // or "caps: not found" on /bin/sh.
    proc.send_keys("caps\n").await.expect("send caps");

    // Collect lines for up to 5s, looking for stsh vs /bin/sh signature.
    let deadline = tokio::time::Instant::now() + Duration::from_secs(5);
    let mut trace: Vec<String> = Vec::new();
    let mut saw_stsh_caps = false;
    let mut saw_sh_not_found = false;

    loop {
        match tokio::time::timeout_at(deadline, stream.next_line()).await {
            Ok(Some(line)) => {
                trace.push(line.clone());
                // stsh `caps` prints space-separated CAP_KIND(rights) tokens;
                // a baseline grant always includes IPC(R).
                if line.contains("IPC(") || line.contains("VFS_OPEN(") {
                    saw_stsh_caps = true;
                    break;
                }
                // /bin/sh's "not found" path.
                if line.contains("caps: not found") || line.contains("not found") {
                    saw_sh_not_found = true;
                    break;
                }
            }
            _ => break,
        }
    }

    let _ = proc.kill().await;

    if !saw_stsh_caps {
        eprintln!("--- trace ({} lines) ---", trace.len());
        for l in &trace {
            eprintln!("  {l}");
        }
        if saw_sh_not_found {
            panic!("login spawned /bin/sh instead of /bin/stsh \
                    (caps builtin not found — wrong shell)");
        } else {
            panic!("could not determine which shell ran — `caps` produced \
                    no recognizable output (see trace above)");
        }
    }
}
