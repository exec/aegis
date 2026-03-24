#!/usr/bin/env python3
"""Phase 25 network protocol stack test.

Boots the ISO with QEMU q35 + virtio-net + SLIRP user networking and verifies:
  1. [NET] configured: 10.0.2.15/24 gw 10.0.2.2
  2. [NET] ICMP: echo reply from 10.0.2.2
     (kernel sends ICMP ping to SLIRP gateway during net_init; QEMU SLIRP
      responds to ICMP natively without any host-side configuration)
"""
import subprocess, sys, os, select, fcntl

QEMU         = "qemu-system-x86_64"
ISO          = "build/aegis.iso"
DISK         = "build/disk.img"
BOOT_TIMEOUT = 900
CMD_TIMEOUT  = 120

def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)

def main():
    if not os.path.exists(ISO):
        fail(f"{ISO} not found — run 'make' first")
    if not os.path.exists(DISK):
        fail(f"{DISK} not found — run 'make disk' first")

    cmd = [
        QEMU,
        "-machine", "q35",
        "-m", "256M",
        "-cdrom", ISO,
        "-drive", f"file={DISK},if=none,id=nvme0,format=raw",
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

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    fd = proc.stdout.fileno()
    fl = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)

    output = ""
    found_configured = False
    found_icmp_reply = False
    import time
    start = time.time()

    try:
        while time.time() - start < BOOT_TIMEOUT:
            r, _, _ = select.select([proc.stdout], [], [], 1.0)
            if r:
                chunk = proc.stdout.read(4096)
                if chunk:
                    text = chunk.decode("utf-8", errors="replace")
                    output += text
                    for line in text.splitlines():
                        print(line)
                        if "[NET] configured:" in line:
                            found_configured = True
                        if "[NET] ICMP: echo reply from 10.0.2.2" in line:
                            found_icmp_reply = True
                    if found_icmp_reply:
                        break
            if proc.poll() is not None:
                break
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    if not found_configured:
        fail("'[NET] configured:' not found in serial output")
    if not found_icmp_reply:
        fail("'[NET] ICMP: echo reply from 10.0.2.2' not found in serial output")

    print("PASS: network stack ICMP self-test succeeded")
    sys.exit(0)

if __name__ == "__main__":
    main()
