#!/usr/bin/env python3
"""Phase 22 xHCI USB controller test.

Boots the shell ISO on QEMU q35 with an xHCI controller and a USB keyboard
attached.  The test verifies:

  1. [XHCI] OK: appears in serial output — driver initialized without crashing.
  2. The shell prompt appears — kernel reached user space with USB devices active.

Notes on keyboard interaction:
  When QEMU has a USB keyboard device present, the monitor 'sendkey' command
  routes keystrokes to the USB keyboard, not the PS/2 keyboard.  The Aegis
  USB HID driver processes USB HID boot reports injected via the xHCI event
  ring; QEMU's sendkey path goes through a different mechanism (HID descriptor
  injection) that is not guaranteed to reach the same event ring.  Because
  of this, interactive shell echo tests are omitted here — the critical
  validation is that the xHCI driver initializes successfully ([XHCI] OK:)
  without crashing the kernel.
"""
import subprocess, time, sys, os, select, fcntl, re, tempfile

QEMU         = "qemu-system-x86_64"
ISO          = "build/aegis-test.iso"
BOOT_TIMEOUT = int(os.environ.get("BOOT_TIMEOUT", "900"))


def build_iso():
    """No-op: aegis-test.iso built by 'make test' upfront."""
    if not os.path.exists(ISO):
        print("[FAIL] %s not found — run 'make test' to build" % ISO)
        sys.exit(1)


def _read_until_prompt_or_timeout(proc, deadline):
    """Read serial bytes from proc.stdout until '# ' is seen or deadline.

    Uses os.read() on the raw fd with O_NONBLOCK to bypass Python's
    BufferedReader (same rationale as test_pipe.py).
    """
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


def run_xhci_boot():
    """Boot QEMU (q35 + xHCI + USB keyboard) with shell.

    Waits for the shell prompt then sends 'exit' via the QEMU monitor to
    trigger the isa-debug-exit shutdown path.

    Returns (serial_output, kernel_lines) where kernel_lines is the list of
    lines starting with '[' (stripped of ANSI sequences).
    """
    mon_path = tempfile.mktemp(suffix=".sock", prefix="aegis_xhci_mon_")
    cmd = [
        QEMU,
        "-machine", "q35", "-cpu", "Broadwell",
        "-cdrom", ISO, "-boot", "order=d",
        "-display", "none", "-vga", "std",
        "-nodefaults", "-serial", "stdio",
        "-no-reboot", "-m", "2G",
        "-device", "qemu-xhci",
        "-device", "usb-kbd",
        "-monitor", "unix:%s,server,nowait" % mon_path,
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
    ]
    proc = subprocess.Popen(
        cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL
    )

    # Wait for QEMU monitor socket to appear
    deadline = time.time() + BOOT_TIMEOUT
    while time.time() < deadline:
        if os.path.exists(mon_path):
            break
        time.sleep(0.1)
    else:
        proc.kill()
        raise RuntimeError("QEMU monitor socket never appeared: %s" % mon_path)

    # Wait for shell prompt
    boot_out = _read_until_prompt_or_timeout(proc, time.time() + BOOT_TIMEOUT)

    # Send quit command via monitor to cleanly exit QEMU
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

    # Extract kernel lines (start with '[') after stripping ANSI sequences
    stripped = re.sub(r'\x1b\[[?0-9;]*[A-Za-z]|\x1bc', '', boot_out)
    kernel_lines = [l for l in stripped.splitlines() if l.startswith('[')]
    return boot_out, kernel_lines


def test_xhci_init():
    """Verify xHCI driver initializes and kernel reaches user space.

    Checks:
      - [XHCI] OK: appears in serial output (driver initialized)
      - Shell prompt '# ' appears (kernel reached user space without crashing)
    """
    print("  booting with q35 + qemu-xhci + usb-kbd ...")
    out, klines = run_xhci_boot()

    xhci_ok = any('[XHCI] OK:' in l for l in klines)
    if not xhci_ok:
        print("FAIL test_xhci_init: [XHCI] OK: not found in serial output")
        print("--- kernel lines ---")
        for l in klines:
            print(l)
        raise AssertionError("[XHCI] OK: not found in serial output\n%s" % out)
    print("  [XHCI] OK: confirmed in serial output")

    shell_ok = "# " in out or "\n#" in out
    if not shell_ok:
        raise AssertionError(
            "FAIL test_xhci_init: shell prompt not found (kernel may have "
            "crashed after xHCI init)\n%s" % out)
    print("  shell prompt confirmed (kernel reached user space)")

    print("PASS test_xhci_init")


if __name__ == "__main__":
    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    build_iso()
    tests = [
        test_xhci_init,
    ]
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
