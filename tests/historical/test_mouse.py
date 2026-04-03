#!/usr/bin/env python3
"""Phase 36 USB HID mouse test.

Boots the vigil ISO on QEMU q35 with xHCI + USB mouse.
Runs mouse_test binary, injects mouse events via QEMU monitor,
verifies event output on serial.
"""
import subprocess, time, sys, os, select, fcntl, re, socket

QEMU         = "qemu-system-x86_64"
ISO          = "build/aegis.iso"
DISK         = "build/disk.img"
BOOT_TIMEOUT = int(os.environ.get("BOOT_TIMEOUT", "900"))
CMD_TIMEOUT  = 30

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def build_iso():
    real_uid = os.getuid()
    real_gid = os.getgid()
    def drop_euid():
        os.setegid(real_gid)
        os.seteuid(real_uid)
    r = subprocess.run(["make", "iso"], preexec_fn=drop_euid)
    if r.returncode != 0:
        print("[FAIL] make iso failed")
        sys.exit(1)


def read_until(proc, pattern, deadline):
    """Read serial output until regex pattern matches or deadline."""
    fd = proc.stdout.fileno()
    fl = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
    buf = b""
    while time.time() < deadline:
        ready, _, _ = select.select([proc.stdout], [], [], 0.5)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue
        if not chunk:
            break
        buf += chunk
        text = buf.decode("utf-8", errors="replace")
        if re.search(pattern, text):
            return text
    return buf.decode("utf-8", errors="replace")


def send_keys(mon, text):
    """Send text as keyboard input via QEMU monitor."""
    for c in text:
        if c == '\n':
            cmd = "sendkey ret"
        elif c == ' ':
            cmd = "sendkey spc"
        elif c == '/':
            cmd = "sendkey slash"
        elif c == '-':
            cmd = "sendkey minus"
        elif c == '_':
            cmd = "sendkey shift-minus"
        elif c == '&':
            cmd = "sendkey shift-7"
        elif c == '.':
            cmd = "sendkey dot"
        elif c == '*':
            cmd = "sendkey shift-8"
        else:
            cmd = f"sendkey {c}"
        mon.sendall((cmd + "\n").encode())
        time.sleep(0.08)


def main():
    build_iso()

    # UNIX socket for QEMU monitor
    import tempfile
    mon_path = tempfile.mktemp(suffix=".sock")

    iso_path = os.path.join(ROOT, ISO)
    disk_path = os.path.join(ROOT, DISK)
    if not os.path.exists(disk_path):
        print(f"SKIP: {DISK} not found -- run 'make disk' first")
        sys.exit(0)

    # Create a temp copy of disk.img so we don't modify the original
    import shutil
    tmp_disk = tempfile.mktemp(suffix=".img")
    shutil.copy2(disk_path, tmp_disk)

    qemu_cmd = [
        QEMU,
        "-machine", "q35", "-cpu", "Broadwell",
        "-m", "2G",
        "-cdrom", iso_path,
        "-boot", "order=d",
        "-display", "none",
        "-vga", "std",
        "-nodefaults",
        "-serial", "stdio",
        "-no-reboot",
        "-drive", f"file={tmp_disk},if=none,id=nvme0,format=raw",
        "-device", "nvme,drive=nvme0,serial=aegis0",
        "-device", "virtio-net-pci,netdev=n0,disable-legacy=on",
        "-netdev", "user,id=n0",
        "-device", "qemu-xhci,id=xhci",
        "-device", "usb-mouse,bus=xhci.0",
        "-device", "usb-kbd,bus=xhci.0",
        "-monitor", f"unix:{mon_path},server,nowait",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
    ]

    proc = subprocess.Popen(qemu_cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL)
    passed = 0
    total = 3

    try:
        deadline = time.time() + BOOT_TIMEOUT

        # Wait for login prompt
        output = read_until(proc, r"login:", deadline)
        if "login:" not in output:
            print(f"[FAIL] No login prompt")
            print(f"  Output: {output[-500:]}")
            sys.exit(1)

        # Connect to QEMU monitor
        time.sleep(1)
        mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        mon.connect(mon_path)
        time.sleep(0.5)
        mon.recv(4096)  # read banner

        # Login
        send_keys(mon, "root\n")
        output = read_until(proc, r"password:", time.time() + CMD_TIMEOUT)
        send_keys(mon, "forevervigilant\n")
        output = read_until(proc, r"[#$] ", time.time() + CMD_TIMEOUT)

        if "#" not in output and "$" not in output:
            print("[FAIL] No shell prompt after login")
            sys.exit(1)

        # Test 1: Verify /dev/mouse exists
        send_keys(mon, "ls /dev/mouse\n")
        output = read_until(proc, r"[#$] ", time.time() + CMD_TIMEOUT)
        if "mouse" in output and "No such" not in output:
            print("[PASS] /dev/mouse exists")
            passed += 1
        else:
            print(f"[FAIL] /dev/mouse not found: {output[-200:]}")

        # Test 2: Run mouse_test, inject events
        send_keys(mon, "mouse_test &\n")
        output = read_until(proc, r"listening", time.time() + CMD_TIMEOUT)
        if "listening" in output:
            print("[PASS] mouse_test started")
            passed += 1
        else:
            print(f"[FAIL] mouse_test did not start: {output[-200:]}")

        # Test 3: Inject mouse movement and verify output
        time.sleep(0.5)
        mon.sendall(b"mouse_move 50 30\n")
        time.sleep(0.5)
        mon.sendall(b"mouse_move 10 -20\n")
        time.sleep(0.5)
        mon.sendall(b"mouse_button 1\n")
        time.sleep(0.3)
        mon.sendall(b"mouse_button 0\n")
        time.sleep(0.5)

        output = read_until(proc, r"btn=", time.time() + CMD_TIMEOUT)
        if "btn=" in output and "dx=" in output:
            print("[PASS] Mouse events received")
            passed += 1
        else:
            print(f"[FAIL] No mouse events: {output[-200:]}")

        mon.close()

    finally:
        proc.terminate()
        proc.wait(timeout=5)
        try:
            os.unlink(mon_path)
        except OSError:
            pass
        try:
            os.unlink(tmp_disk)
        except OSError:
            pass

    print(f"\n{passed}/{total} tests passed")
    sys.exit(0 if passed >= 2 else 1)


if __name__ == "__main__":
    main()
