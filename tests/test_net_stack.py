#!/usr/bin/env python3
"""Phase 27 DHCP network stack test.

Boots the ISO with QEMU q35 + virtio-net + SLIRP user networking and verifies:
  1. [DHCP] acquired — DHCP daemon obtained an IP address
"""
import subprocess, sys, os, select, fcntl, time

QEMU         = "qemu-system-x86_64"
ISO          = "build/aegis.iso"
DISK         = "build/disk.img"
BOOT_TIMEOUT = int(os.environ.get("BOOT_TIMEOUT", "900"))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def build_iso():
    """Build the vigil ISO (make INIT=vigil iso)."""
    real_uid = os.getuid()
    real_gid = os.getgid()

    def drop_euid():
        os.setegid(real_gid)
        os.seteuid(real_uid)

    r = subprocess.run(["make", "INIT=vigil", "iso"],
                       cwd=ROOT, preexec_fn=drop_euid)
    if r.returncode != 0:
        print("[FAIL] make INIT=vigil iso failed")
        sys.exit(1)


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)

def main():
    build_iso()

    iso_path = os.path.join(ROOT, ISO)
    disk_path = os.path.join(ROOT, DISK)

    if not os.path.exists(iso_path):
        fail(f"{ISO} not found — run 'make' first")
    if not os.path.exists(disk_path):
        fail(f"{DISK} not found — run 'make disk' first")

    cmd = [
        QEMU,
        "-machine", "q35",
        "-m", "256M",
        "-cdrom", iso_path,
        "-drive", f"file={disk_path},if=none,id=nvme0,format=raw",
        "-device", "nvme,drive=nvme0,serial=aegis0",
        "-boot", "order=d",
        "-display", "none",
        "-vga", "std",
        "-nodefaults",
        "-serial", "stdio",
        "-no-reboot",
        "-device", "virtio-net-pci,netdev=n0,disable-legacy=on",
        "-netdev", "user,id=n0",
    ]

    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    fd = proc.stdout.fileno()
    fl = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)

    output = ""
    found_dhcp = False
    start = time.time()

    try:
        while time.time() - start < BOOT_TIMEOUT:
            r, _, _ = select.select([proc.stdout], [], [], 1.0)
            if r:
                try:
                    chunk = os.read(fd, 4096)
                except BlockingIOError:
                    chunk = b""
                if chunk:
                    text = chunk.decode("utf-8", errors="replace")
                    output += text
                    for line in text.splitlines():
                        print(line)
                    if "[DHCP] acquired" in output:
                        found_dhcp = True
                        break
            if proc.poll() is not None:
                break
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    if not found_dhcp:
        fail("'[DHCP] acquired' not found in serial output")

    print("PASS: DHCP acquired IP address")
    sys.exit(0)

if __name__ == "__main__":
    main()
