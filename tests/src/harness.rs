use std::path::Path;
use std::time::Duration;
use std::collections::HashMap;
use std::sync::Arc;

use chrono::Utc;
use vortex::{
    ConsoleOutput, ConsoleStream, QemuBackend, QemuProcess, ResourceLimits,
    VmInstance, VmSpec, VmState,
};
use vortex::core::config::{BootSource, ExitMapping, ExitMeaning, QemuOpts};

#[derive(Debug)]
pub enum HarnessError {
    /// Kernel explicitly signalled failure via isa-debug-exit (exit code 35).
    KernelFail(ConsoleOutput),
    /// QEMU failed to start.
    SpawnError(String),
}

impl std::fmt::Display for HarnessError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            HarnessError::KernelFail(out) => write!(
                f,
                "kernel signalled FAIL via isa-debug-exit\n\nCapture:\n{}",
                out.kernel_lines.join("\n")
            ),
            HarnessError::SpawnError(msg) => write!(f, "QEMU spawn error: {msg}"),
        }
    }
}

fn boot_timeout() -> Duration {
    let secs = std::env::var("AEGIS_BOOT_TIMEOUT")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(30u64);
    Duration::from_secs(secs)
}

fn make_vm(opts: QemuOpts, iso: &Path) -> VmInstance {
    make_vm_from_boot_source(
        "aegis-test",
        opts,
        BootSource::Cdrom { iso: iso.to_path_buf() },
    )
}

fn make_vm_from_boot_source(
    id: &str,
    opts: QemuOpts,
    boot_source: BootSource,
) -> VmInstance {
    VmInstance {
        id: id.into(),
        spec: VmSpec {
            image: String::new(),
            memory: 2048,
            cpus: 1,
            ports: HashMap::new(),
            volumes: HashMap::new(),
            environment: HashMap::new(),
            command: None,
            labels: HashMap::new(),
            network_config: None,
            resource_limits: ResourceLimits::default(),
            backend: Some("qemu".into()),
            boot_source: Some(boot_source),
            qemu_opts: Some(opts),
            exit_mappings: vec![
                ExitMapping { raw: 33, meaning: ExitMeaning::Pass },
                ExitMapping { raw: 35, meaning: ExitMeaning::Fail },
            ],
        },
        state: VmState::Stopped,
        backend: Arc::new(QemuBackend::new()),
        created_at: Utc::now(),
        updated_at: Utc::now(),
    }
}

pub struct AegisHarness;

impl AegisHarness {
    /// Boot QEMU, collect all serial output, return when either QEMU exits or
    /// AEGIS_BOOT_TIMEOUT (default 30s) elapses. QEMU is killed on timeout.
    pub async fn boot(opts: QemuOpts, iso: &Path) -> Result<ConsoleOutput, HarnessError> {
        let vm = make_vm(opts, iso);
        Self::run_until_deadline(vm).await
    }

    /// Boot QEMU with a raw kernel ELF via `-kernel` instead of an ISO.
    ///
    /// Used by the ARM64 boot oracle, which has no bootable ISO image —
    /// QEMU loads the ELF directly via `BootSource::Kernel`. The caller
    /// must set `VORTEX_QEMU_BINARY=qemu-system-aarch64` before invoking
    /// this method, since Vortex reads that env var once when
    /// `QemuBackend::new()` runs inside `make_vm_from_boot_source`.
    ///
    /// Capture + timeout semantics are identical to `boot()`: collect
    /// serial output until QEMU exits or `AEGIS_BOOT_TIMEOUT` elapses,
    /// killing the process on timeout. The ARM64 kernel currently has
    /// no way to signal "test done" from inside the VM, so callers must
    /// set `AEGIS_BOOT_TIMEOUT` low enough that the test doesn't stall.
    pub async fn boot_kernel(
        opts: QemuOpts,
        kernel: &Path,
    ) -> Result<ConsoleOutput, HarnessError> {
        let vm = make_vm_from_boot_source(
            "aegis-arm64-test",
            opts,
            BootSource::Kernel {
                kernel: kernel.to_path_buf(),
                initrd: None,
                cmdline: None,
            },
        );
        Self::run_until_deadline(vm).await
    }

    /// Spawn QEMU, drain the serial channel until the deadline, reap.
    ///
    /// Shared implementation between `boot()` and `boot_kernel()`.
    async fn run_until_deadline(vm: VmInstance) -> Result<ConsoleOutput, HarnessError> {
        let backend = QemuBackend::new();
        let mut proc = backend
            .spawn(&vm)
            .map_err(|e| HarnessError::SpawnError(e.to_string()))?;

        let mut stream = proc
            .console
            .take()
            .expect("serial_capture must be true in QemuOpts");

        let deadline = tokio::time::Instant::now() + boot_timeout();
        let mut output = ConsoleOutput::default();

        loop {
            match tokio::time::timeout_at(deadline, stream.next_line()).await {
                Ok(Some(line)) => {
                    if line.starts_with('[') {
                        output.kernel_lines.push(line.clone());
                    }
                    output.lines.push(line);
                }
                Ok(None) => break,   // QEMU exited
                Err(_) => {          // deadline hit
                    let _ = proc.kill().await;
                    break;
                }
            }
        }

        // Reap process; check for explicit kernel failure signal.
        if let Ok(status) = proc.wait().await {
            if let Some(ExitMeaning::Fail) = status.apply_mappings(&vm.spec.exit_mappings) {
                return Err(HarnessError::KernelFail(output));
            }
        }

        Ok(output)
    }

    /// Boot QEMU and return live stream + process handle.
    /// Caller is responsible for killing the process after use.
    pub async fn boot_stream(
        opts: QemuOpts,
        iso: &Path,
    ) -> Result<(ConsoleStream, QemuProcess), HarnessError> {
        let vm = make_vm(opts, iso);
        let backend = QemuBackend::new();
        let mut proc = backend
            .spawn(&vm)
            .map_err(|e| HarnessError::SpawnError(e.to_string()))?;
        let stream = proc
            .console
            .take()
            .expect("serial_capture must be true in QemuOpts");
        Ok((stream, proc))
    }

    /// Boot QEMU with a persistent disk image, NO ISO.
    ///
    /// Use this for the second boot of a two-boot test sequence —
    /// the disk image was populated by the first boot and must be
    /// loaded back with the same file path. The caller-supplied
    /// `opts` determines everything else (machine, devices, drives,
    /// extra args). `opts.drives` should contain the `-drive ...`
    /// entries for the NVMe image (and any pflash firmware); this
    /// function does not inject anything into `opts`.
    ///
    /// Returns the same (ConsoleStream, QemuProcess) tuple as
    /// `boot_stream`. Caller is responsible for killing the process
    /// and managing the disk file's lifetime.
    pub async fn boot_disk_only(
        opts: QemuOpts,
    ) -> Result<(ConsoleStream, QemuProcess), HarnessError> {
        let vm = VmInstance {
            id: "aegis-installed".into(),
            spec: VmSpec {
                image: String::new(),
                memory: 2048,
                cpus: 1,
                ports: HashMap::new(),
                volumes: HashMap::new(),
                environment: HashMap::new(),
                command: None,
                labels: HashMap::new(),
                network_config: None,
                resource_limits: ResourceLimits::default(),
                backend: Some("qemu".into()),
                /* No boot source — boot comes from pflash (UEFI) +
                 * NVMe drive specified in opts.drives. Vortex's
                 * BootSource enum has no "none" variant, so we pass
                 * a fake Cdrom pointing at /dev/null which QEMU
                 * ignores when -boot order overrides it via extra
                 * args. */
                boot_source: Some(BootSource::Cdrom {
                    iso: std::path::PathBuf::from("/dev/null"),
                }),
                qemu_opts: Some(opts),
                exit_mappings: vec![
                    ExitMapping { raw: 33, meaning: ExitMeaning::Pass },
                    ExitMapping { raw: 35, meaning: ExitMeaning::Fail },
                ],
            },
            state: VmState::Stopped,
            backend: Arc::new(QemuBackend::new()),
            created_at: Utc::now(),
            updated_at: Utc::now(),
        };
        let backend = QemuBackend::new();
        let mut proc = backend
            .spawn(&vm)
            .map_err(|e| HarnessError::SpawnError(e.to_string()))?;
        let stream = proc
            .console
            .take()
            .expect("serial_capture must be true in QemuOpts");
        Ok((stream, proc))
    }
}
