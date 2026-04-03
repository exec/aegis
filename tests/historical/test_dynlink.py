#!/usr/bin/env python3
"""test_dynlink.py — Phase 33 dynamic linking integration test.

Boots Aegis with q35 + NVMe disk containing dynamically-linked binaries
and /lib/libc.so. Logs in, runs several dynamic binaries including
dynlink_test which validates interpreter mapping in /proc/self/maps.

Skipped if build/disk.img is not present.
"""
import subprocess, sys, os, select, fcntl, time, socket, tempfile

QEMU         = "qemu-system-x86_64"
ISO          = "build/aegis.iso"
DISK         = "build/disk.img"
BOOT_TIMEOUT = 120
CMD_TIMEOUT  = 30

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

_KEY_MAP = {
    ' ': 'spc', '\n': 'ret', '/': 'slash', '-': 'minus', '.': 'dot',
    ':': 'shift-semicolon', '|': 'shift-backslash', '_': 'shift-minus',
}
for c in 'abcdefghijklmnopqrstuvwxyz': _KEY_MAP[c] = c
for c in '0123456789': _KEY_MAP.setdefault(c, c)


def _type_string(mon_sock, s):
    for ch in s:
        key = _KEY_MAP.get(ch)
        if key is None:
            continue
        mon_sock.sendall(f'sendkey {key}\n'.encode())
        time.sleep(0.08)
        try:
            mon_sock.recv(4096)
        except OSError:
            pass


class SerialReader:
    def __init__(self, fd):
        self._fd = fd
        self._buf = b""

    def _drain(self, timeout=0.5):
        ready, _, _ = select.select([self._fd], [], [], timeout)
        if ready:
            try:
                chunk = os.read(self._fd, 65536)
                if chunk:
                    self._buf += chunk
            except (BlockingIOError, OSError):
                pass

    def wait_for(self, needle, deadline):
        enc = needle.encode()
        while time.time() < deadline:
            if enc in self._buf:
                return True
            self._drain()
        return enc in self._buf

    def full_output(self):
        return self._buf.decode("utf-8", errors="replace")


def build_iso():
    r = subprocess.run("make INIT=vigil iso", shell=True,
                       cwd=ROOT, capture_output=True)
    if r.returncode != 0:
        print("[FAIL] make INIT=vigil iso failed")
        print(r.stderr.decode())
        sys.exit(1)


def run_test():
    iso_path  = os.path.join(ROOT, ISO)
    disk_path = os.path.join(ROOT, DISK)

    if not os.path.exists(disk_path):
        print(f"SKIP: {DISK} not found — run 'make disk' first")
        sys.exit(0)

    build_iso()

    mon_path = tempfile.mktemp(suffix=".sock")
    proc = subprocess.Popen(
        [QEMU,
         "-machine", "q35", "-cpu", "Broadwell",
         "-cdrom", iso_path, "-boot", "order=d",
         "-display", "none", "-vga", "std", "-nodefaults",
         "-serial", "stdio", "-no-reboot", "-m", "2G",
         "-drive", f"file={disk_path},if=none,id=nvme0,format=raw",
         "-device", "nvme,drive=nvme0,serial=aegis0",
         "-device", "virtio-net-pci,netdev=n0,disable-legacy=on",
         "-netdev", "user,id=n0",
         "-monitor", f"unix:{mon_path},server,nowait"],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    flags = fcntl.fcntl(proc.stdout.fileno(), fcntl.F_GETFL)
    fcntl.fcntl(proc.stdout.fileno(), fcntl.F_SETFL, flags | os.O_NONBLOCK)
    serial = SerialReader(proc.stdout.fileno())

    deadline = time.time() + 10
    while not os.path.exists(mon_path) and time.time() < deadline:
        time.sleep(0.1)
    mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    mon.connect(mon_path)
    mon.setblocking(False)

    errors = []
    try:
        # Login
        if not serial.wait_for("login: ", time.time() + BOOT_TIMEOUT):
            print("FAIL: login prompt not found")
            sys.exit(1)
        _type_string(mon, "root\n")

        if not serial.wait_for("assword", time.time() + 10):
            print("FAIL: password prompt not found")
            sys.exit(1)
        _type_string(mon, "forevervigilant\n")

        if not serial.wait_for("# ", time.time() + 10):
            print("FAIL: shell prompt not found")
            sys.exit(1)

        # Test 1: Run a simple dynamic binary (echo)
        time.sleep(1)
        _type_string(mon, "/bin/echo dynlink-test-1\n")
        if serial.wait_for("dynlink-test-1", time.time() + CMD_TIMEOUT):
            print("  PASS: /bin/echo (dynamic) works")
        else:
            errors.append("FAIL: /bin/echo did not produce output")

        # Test 2: Run cat on a file
        time.sleep(0.5)
        _type_string(mon, "/bin/cat /etc/motd\n")
        if serial.wait_for("Welcome to Aegis", time.time() + CMD_TIMEOUT):
            print("  PASS: /bin/cat (dynamic) works")
        else:
            errors.append("FAIL: /bin/cat /etc/motd did not produce expected output")

        # Test 3: ls /lib — verify libc.so is visible
        time.sleep(0.5)
        _type_string(mon, "/bin/ls /lib\n")
        if serial.wait_for("libc.so", time.time() + CMD_TIMEOUT):
            print("  PASS: /bin/ls /lib shows libc.so")
        else:
            errors.append("FAIL: /bin/ls /lib did not show libc.so")

        # Test 4: Run the dynamic linking test binary
        time.sleep(0.5)
        _type_string(mon, "/bin/dynlink_test\n")
        if serial.wait_for("DYNLINK OK", time.time() + CMD_TIMEOUT):
            print("  PASS: dynlink_test reports DYNLINK OK")
        else:
            errors.append("FAIL: dynlink_test did not report DYNLINK OK")
            print(f"  last 1000 chars: {serial.full_output()[-1000:]!r}")

    finally:
        try:
            mon.close()
        except OSError:
            pass
        proc.kill()
        proc.wait()
        try:
            os.unlink(mon_path)
        except OSError:
            pass

    if errors:
        for e in errors:
            print(e)
        sys.exit(1)

    print("PASS test_dynlink")
    sys.exit(0)


if __name__ == "__main__":
    run_test()
