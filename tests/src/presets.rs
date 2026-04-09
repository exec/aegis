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
}
