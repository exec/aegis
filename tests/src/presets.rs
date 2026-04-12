use std::path::PathBuf;
use vortex::core::config::QemuOpts;

pub fn iso() -> PathBuf {
    let val = std::env::var("AEGIS_ISO").unwrap_or_else(|_| "build/aegis.iso".into());
    PathBuf::from(val)
}

pub fn disk() -> PathBuf {
    let val = std::env::var("AEGIS_DISK").unwrap_or_else(|_| "build/disk.img".into());
    PathBuf::from(val)
}

/// Path to the ARM64 kernel ELF, overridable via `AEGIS_ARM64_ELF`.
///
/// Name is `arm64_elf` (rather than just `elf`) so it's unambiguous
/// when we later have multiple kernel image formats.
pub fn arm64_elf() -> PathBuf {
    let val = std::env::var("AEGIS_ARM64_ELF")
        .unwrap_or_else(|_| "kernel/arch/arm64/build/aegis-arm64.elf".into());
    PathBuf::from(val)
}

pub fn aegis_pc() -> QemuOpts {
    QemuOpts {
        machine: "pc".into(),
        display: "none".into(),
        devices: vec![],
        drives: vec![],
        extra_args: vec![
            "-nodefaults".into(),
            "-cpu".into(), "Broadwell".into(),
            "-vga".into(), "std".into(),
            "-no-reboot".into(),
            "-device".into(), "isa-debug-exit,iobase=0xf4,iosize=0x04".into(),
        ],
        serial_capture: true,
        monitor_socket: false,
    }
}

pub fn aegis_q35() -> QemuOpts {
    QemuOpts {
        machine: "q35".into(),
        display: "none".into(),
        devices: vec![
            "qemu-xhci,id=xhci".into(),
            "usb-kbd,bus=xhci.0".into(),
            "usb-mouse,bus=xhci.0".into(),
            "nvme,drive=nvme0,serial=aegis0".into(),
            "virtio-net-pci,netdev=net0".into(),
        ],
        drives: vec![
            format!("file={},format=raw,if=none,id=nvme0", disk().display()),
        ],
        extra_args: vec![
            "-nodefaults".into(),
            "-cpu".into(), "Broadwell".into(),
            "-netdev".into(),
            "user,id=net0,hostfwd=tcp::2222-:22,hostfwd=tcp::8080-:80".into(),
            "-vga".into(), "std".into(),
            "-no-reboot".into(),
            "-device".into(), "isa-debug-exit,iobase=0xf4,iosize=0x04".into(),
        ],
        serial_capture: true,
        monitor_socket: false,
    }
}

/// Graphical q35 preset with framebuffer + monitor socket + PS/2 KBD/mouse.
///
/// Differs from `aegis_q35` in: uses virtio-vga instead of std VGA,
/// enables `monitor_socket` so tests can drive HMP `screendump` /
/// `sendkey` / `mouse_move` / `mouse_button`, omits NVMe/virtio-net,
/// and omits both `usb-kbd` and `usb-mouse`. HMP `sendkey` and
/// `mouse_move` route to the first keyboard / pointing device on the
/// machine, so a USB HID device would intercept events that need to
/// reach the Aegis PS/2 drivers (/dev/kbd and /dev/mouse). q35
/// exposes PS/2 keyboard and AUX via the ICH9 LPC i8042 by default.
/// Intended for GUI tests that need to drive login + clicks and
/// capture frames.
pub fn aegis_q35_graphical_mouse() -> QemuOpts {
    QemuOpts {
        machine: "q35".into(),
        // `to=99` lets QEMU walk forward from display :17 to :99 if a
        // stale instance is still bound — avoids "Address already in
        // use" when a prior test panicked before killing its child.
        display: "vnc=127.0.0.1:17,to=99".into(),
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

/// q35 preset for installer tests: ISO + persistent NVMe drive + monitor socket.
///
/// The caller supplies the disk image path; this function does not
/// create or delete it. Used for Boot 1 of the installer test (ISO
/// boot with empty NVMe attached for installation).
///
/// No usb-kbd: HMP `sendkey` routes to the first keyboard device;
/// with usb-kbd present it intercepts keys the PS/2 kernel driver
/// expects. q35 LPC provides a default PS/2 keyboard.
pub fn aegis_q35_installer(disk_path: &std::path::Path) -> QemuOpts {
    QemuOpts {
        machine: "q35".into(),
        display: "none".into(),
        devices: vec![
            "nvme,drive=nvme0,serial=aegis0".into(),
        ],
        drives: vec![
            format!("file={},if=none,id=nvme0,format=raw", disk_path.display()),
        ],
        extra_args: vec![
            "-cpu".into(), "Broadwell".into(),
            "-no-reboot".into(),
            "-nodefaults".into(),
            "-vga".into(), "std".into(),
        ],
        serial_capture: true,
        monitor_socket: true,
    }
}

/// q35 preset for GUI installer tests: ISO + NVMe drive + virtio-vga + monitor socket.
///
/// Differs from `aegis_q35_installer` in that it uses `virtio-vga`
/// instead of `-vga std` so that Aegis's `sys_fb_map` syscall can
/// actually map a framebuffer — the GUI installer refuses to start
/// otherwise. Same nvme0 drive, same monitor socket, no usb-kbd
/// (HMP `sendkey` must reach the PS/2 keyboard driver).
pub fn aegis_q35_gui_installer(disk_path: &std::path::Path) -> QemuOpts {
    QemuOpts {
        machine: "q35".into(),
        display: "vnc=127.0.0.1:19,to=99".into(),
        devices: vec![
            "virtio-vga".into(),
            "nvme,drive=nvme0,serial=aegis0".into(),
        ],
        drives: vec![
            format!("file={},if=none,id=nvme0,format=raw", disk_path.display()),
        ],
        extra_args: vec![
            "-cpu".into(), "Broadwell".into(),
            "-no-reboot".into(),
        ],
        serial_capture: true,
        monitor_socket: true,
    }
}

/// ARM64 `virt` machine preset — no ISO, kernel ELF boot via `-kernel`.
///
/// Used by the ARM64 boot oracle. This preset is intentionally
/// minimal: PL011 UART on stdio, 2 GiB RAM, cortex-a72 CPU, no
/// display, no devices. No `isa-debug-exit` — that device is x86
/// specific and the ARM64 kernel currently has no way to signal
/// exit, so the harness relies on `AEGIS_BOOT_TIMEOUT` to stop the
/// VM once serial capture is complete.
///
/// The caller must set `VORTEX_QEMU_BINARY=qemu-system-aarch64`
/// before constructing the harness, since Vortex reads that env
/// var once when `QemuBackend::new()` runs.
pub fn aegis_arm64_virt() -> QemuOpts {
    QemuOpts {
        machine: "virt".into(),
        display: "none".into(),
        devices: vec![],
        drives: vec![],
        // Do NOT add `-serial stdio` here — Vortex's QemuBackend
        // injects that automatically when `serial_capture = true`, and
        // QEMU rejects duplicate `-serial stdio` with "cannot use
        // stdio by multiple character devices". Same for `-m`, which
        // Vortex emits from `VmSpec::memory` (set to 2048 MiB in the
        // harness).
        extra_args: vec![
            "-nodefaults".into(),
            "-cpu".into(), "cortex-a72".into(),
            "-no-reboot".into(),
        ],
        serial_capture: true,
        monitor_socket: false,
    }
}

/// q35 preset for post-installer boot: NVMe drive only, no ISO, OVMF UEFI firmware.
///
/// Used for Boot 2 of the installer test to verify the installed
/// system boots standalone via UEFI. `disk_path` is the same path
/// that was used in the preceding `aegis_q35_installer` boot.
/// `ovmf_path` is the OVMF_CODE firmware binary (typically
/// `/usr/share/OVMF/OVMF_CODE_4M.fd` on Debian-derived systems).
pub fn aegis_q35_installed_ovmf(disk_path: &std::path::Path,
                                 ovmf_path: &std::path::Path) -> QemuOpts {
    QemuOpts {
        machine: "q35".into(),
        display: "none".into(),
        devices: vec![
            "nvme,drive=nvme0,serial=aegis0".into(),
        ],
        drives: vec![
            format!("if=pflash,format=raw,readonly=on,file={}", ovmf_path.display()),
            format!("file={},if=none,id=nvme0,format=raw", disk_path.display()),
        ],
        extra_args: vec![
            "-cpu".into(), "Broadwell".into(),
            "-no-reboot".into(),
            "-nodefaults".into(),
            "-vga".into(), "std".into(),
        ],
        serial_capture: true,
        monitor_socket: true,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pc_preset_machine_and_serial() {
        let opts = aegis_pc();
        assert_eq!(opts.machine, "pc");
        assert!(opts.serial_capture);
        assert_eq!(opts.display, "none");
    }

    #[test]
    fn pc_preset_has_isa_debug_exit() {
        let opts = aegis_pc();
        let args = opts.extra_args.join(" ");
        assert!(args.contains("isa-debug-exit"), "missing isa-debug-exit in: {args}");
        assert!(args.contains("0xf4"), "missing iobase in: {args}");
    }

    #[test]
    fn pc_preset_no_devices() {
        let opts = aegis_pc();
        assert!(opts.devices.is_empty());
        assert!(opts.drives.is_empty());
    }

    #[test]
    fn q35_preset_machine_and_serial() {
        let opts = aegis_q35();
        assert_eq!(opts.machine, "q35");
        assert!(opts.serial_capture);
    }

    #[test]
    fn q35_preset_has_nvme_and_xhci() {
        let opts = aegis_q35();
        let devices = opts.devices.join(" ");
        assert!(devices.contains("qemu-xhci"), "missing xhci in: {devices}");
        assert!(devices.contains("nvme"), "missing nvme in: {devices}");
        assert!(devices.contains("virtio-net-pci"), "missing virtio-net in: {devices}");
    }

    #[test]
    fn q35_preset_has_drive() {
        let opts = aegis_q35();
        assert!(!opts.drives.is_empty(), "q35 must have at least one drive");
        assert!(opts.drives[0].contains("nvme0"), "drive must be named nvme0");
    }

    #[test]
    fn pc_preset_has_nodefaults() {
        let opts = aegis_pc();
        assert!(opts.extra_args.contains(&"-nodefaults".to_string()));
    }

    #[test]
    fn pc_preset_has_broadwell_cpu() {
        let opts = aegis_pc();
        let args = opts.extra_args.join(" ");
        assert!(args.contains("-cpu Broadwell"), "missing -cpu Broadwell in: {args}");
    }

    #[test]
    fn q35_preset_has_broadwell_cpu() {
        let opts = aegis_q35();
        let args = opts.extra_args.join(" ");
        assert!(args.contains("-cpu Broadwell"), "missing -cpu Broadwell in: {args}");
    }

    #[test]
    fn q35_preset_has_nodefaults() {
        let opts = aegis_q35();
        assert!(opts.extra_args.contains(&"-nodefaults".to_string()));
    }

    #[test]
    fn q35_preset_has_isa_debug_exit() {
        let opts = aegis_q35();
        let args = opts.extra_args.join(" ");
        assert!(args.contains("isa-debug-exit"), "missing isa-debug-exit in: {args}");
        assert!(args.contains("0xf4"), "missing iobase in: {args}");
    }

    #[test]
    fn arm64_virt_preset_machine_and_serial() {
        let opts = aegis_arm64_virt();
        assert_eq!(opts.machine, "virt");
        assert_eq!(opts.display, "none");
        assert!(opts.serial_capture);
        assert!(!opts.monitor_socket);
    }

    #[test]
    fn arm64_virt_preset_has_cortex_a72_and_no_reboot() {
        let opts = aegis_arm64_virt();
        let args = opts.extra_args.join(" ");
        assert!(args.contains("-cpu cortex-a72"), "missing cortex-a72 in: {args}");
        assert!(args.contains("-no-reboot"), "missing -no-reboot in: {args}");
        assert!(args.contains("-nodefaults"), "missing -nodefaults in: {args}");
    }

    #[test]
    fn arm64_virt_preset_no_duplicate_serial_or_memory() {
        // Vortex auto-injects `-serial stdio` and `-m <memory>`, so
        // neither must appear in the preset's extra_args or QEMU will
        // reject duplicate char device / memory size.
        let opts = aegis_arm64_virt();
        let args = opts.extra_args.join(" ");
        assert!(!args.contains("-serial"),
                "preset must not set -serial (Vortex injects it): {args}");
        assert!(!args.contains("-m "),
                "preset must not set -m (Vortex injects from memory): {args}");
    }

    #[test]
    fn arm64_virt_preset_no_isa_debug_exit() {
        // isa-debug-exit is x86-specific; must not appear in the ARM64 preset.
        let opts = aegis_arm64_virt();
        let args = opts.extra_args.join(" ");
        assert!(!args.contains("isa-debug-exit"));
    }

    #[test]
    fn arm64_virt_preset_no_devices_or_drives() {
        let opts = aegis_arm64_virt();
        assert!(opts.devices.is_empty());
        assert!(opts.drives.is_empty());
    }
}
