#!/usr/bin/env python3
"""Phase 24 virtio-net driver test.

Boots the shell ISO on QEMU q35 with a virtio-net-pci device (modern,
SLIRP user networking). Verifies:

  1. [NET] OK: virtio-net eth0 appears in serial output — driver initialized.
  2. Shell prompt appears — kernel reached user space without crashing.

Phase 24 does not test actual packet transmission (that is Phase 25).
"""
import subprocess, time, sys, os, select, fcntl, re, tempfile

QEMU         = "qemu-system-x86_64"
ISO          = "build/aegis-test.iso"
BOOT_TIMEOUT = int(os.environ.get("BOOT_TIMEOUT", "900"))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def build_iso():
    """No-op: aegis-test.iso built by 'make test' upfront."""
    if not os.path.exists(os.path.join(ROOT, ISO)):
        print("[FAIL] %s not found — run 'make test' to build" % ISO)
        sys.exit(1)


def _read_until_prompt_or_timeout(proc, deadline):
    """Read serial bytes until '# ' or deadline. Non-blocking O_NONBLOCK loop."""
    fd = proc.stdout.fileno()
    fl = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
    buf = b""
    while time.time() < deadline:
        ready, _, _ = select.select([proc.stdout], [], [], 0.1)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue
        if not chunk:
            break
        buf += chunk
        if b"# " in buf:
            return buf.decode(errors="replace")
    return buf.decode(errors="replace")


def boot_qemu_virtio_net():
    """Boot QEMU q35 + virtio-net-pci (modern, SLIRP).

    Returns (serial_output, kernel_lines).
    """
    mon_path = tempfile.mktemp(suffix=".sock", prefix="aegis_net_mon_")
    cmd = [
        QEMU,
        "-machine", "q35", "-cpu", "Broadwell",
        "-cdrom", os.path.join(ROOT, ISO), "-boot", "order=d",
        "-display", "none", "-vga", "std",
        "-nodefaults", "-serial", "stdio",
        "-no-reboot", "-m", "2G",
        "-device", "virtio-net-pci,netdev=n0,disable-legacy=on",
        "-netdev", "user,id=n0",
        "-monitor", "unix:%s,server,nowait" % mon_path,
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
    ]
    proc = subprocess.Popen(
        cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL
    )

    deadline = time.time() + BOOT_TIMEOUT
    while time.time() < deadline:
        if os.path.exists(mon_path):
            break
        time.sleep(0.1)
    else:
        proc.kill()
        raise RuntimeError("QEMU monitor socket never appeared: %s" % mon_path)

    boot_out = _read_until_prompt_or_timeout(proc, time.time() + BOOT_TIMEOUT)

    import socket as _socket
    try:
        mon_sock = _socket.socket(_socket.AF_UNIX, _socket.SOCK_STREAM)
        mon_sock.connect(mon_path)
        mon_sock.setblocking(False)
        mon_sock.sendall(b"quit\n")
        mon_sock.close()
    except OSError:
        pass

    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()

    try:
        os.unlink(mon_path)
    except OSError:
        pass

    stripped = re.sub(r'\x1b\[[?0-9;]*[A-Za-z]|\x1bc', '', boot_out)
    kernel_lines = [l for l in stripped.splitlines() if l.startswith('[')]
    return boot_out, kernel_lines


def test_virtio_net_init():
    """Verify virtio-net driver initializes and kernel reaches user space.

    Checks:
      - [NET] OK: virtio-net eth0 appears in serial output (driver init)
      - Shell prompt '# ' appears (kernel reached user space)
    """
    print("  booting with q35 + virtio-net-pci (disable-legacy=on) + SLIRP ...")
    out, klines = boot_qemu_virtio_net()

    net_ok = any('[NET] OK: virtio-net eth0' in l for l in klines)
    if not net_ok:
        print("FAIL test_virtio_net_init: [NET] OK: virtio-net eth0 not found")
        print("--- kernel lines ---")
        for l in klines:
            print(l)
        raise AssertionError(
            "[NET] OK: virtio-net eth0 not found in serial output\n%s" % out)
    print("  [NET] OK: virtio-net eth0 confirmed in serial output")

    shell_ok = "# " in out or "\n#" in out
    if not shell_ok:
        raise AssertionError(
            "FAIL test_virtio_net_init: shell prompt not found "
            "(kernel may have crashed after virtio-net init)\n%s" % out)
    print("  shell prompt confirmed (kernel reached user space)")

    print("PASS test_virtio_net_init")


if __name__ == "__main__":
    os.chdir(ROOT)
    build_iso()
    tests = [test_virtio_net_init]
    failures = 0
    for t in tests:
        try:
            t()
        except AssertionError as e:
            print(e)
            failures += 1
        except Exception as e:
            print("ERROR in %s: %s" % (t.__name__, e))
            failures += 1
    if failures:
        print("\n%d/%d tests FAILED" % (failures, len(tests)))
        sys.exit(1)
    print("\n%d/%d tests PASSED" % (len(tests), len(tests)))
