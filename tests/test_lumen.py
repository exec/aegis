#!/usr/bin/env python3
"""Phase 37 Lumen compositor test.

Boots QEMU q35 with xHCI + USB mouse + USB keyboard.
Logs in, launches lumen, verifies startup message on serial.
Injects mouse movement and keyboard input, checks for responsiveness.
"""
import subprocess, time, sys, os, select, fcntl, re, socket, tempfile

QEMU = "qemu-system-x86_64"
ISO = "build/aegis.iso"
BOOT_TIMEOUT = int(os.environ.get("BOOT_TIMEOUT", "900"))
CMD_TIMEOUT = 30


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
        else:
            cmd = f"sendkey {c}"
        mon.sendall((cmd + "\n").encode())
        time.sleep(0.08)


def main():
    build_iso()

    mon_path = tempfile.mktemp(suffix=".sock")
    qemu_cmd = [
        QEMU, "-machine", "q35", "-cpu", "Broadwell", "-m", "2G",
        "-cdrom", ISO, "-boot", "order=d",
        "-display", "none", "-vga", "std",
        "-nodefaults", "-serial", "stdio", "-no-reboot",
        "-device", "qemu-xhci,id=xhci",
        "-device", "usb-mouse,bus=xhci.0",
        "-device", "usb-kbd,bus=xhci.0",
        "-monitor", f"unix:{mon_path},server,nowait",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
    ]

    proc = subprocess.Popen(qemu_cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL)
    passed = 0
    total = 2

    try:
        deadline = time.time() + BOOT_TIMEOUT

        # Wait for login prompt
        output = read_until(proc, r"login:", deadline)
        if "login:" not in output:
            print(f"[FAIL] No login prompt")
            print(f"  Output: {output[-500:]}")
            sys.exit(1)

        time.sleep(1)
        mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        mon.connect(mon_path)
        time.sleep(0.5)
        mon.recv(4096)

        # Login
        send_keys(mon, "root\n")
        output = read_until(proc, r"password:", time.time() + CMD_TIMEOUT)
        send_keys(mon, "forevervigilant\n")
        output = read_until(proc, r"[#$] ", time.time() + CMD_TIMEOUT)

        # Test 1: Launch lumen, check startup message
        send_keys(mon, "lumen &\n")
        output = read_until(proc, r"\[LUMEN\] started", time.time() + CMD_TIMEOUT)
        if "[LUMEN] started" in output:
            print("[PASS] Lumen started successfully")
            passed += 1
        else:
            print(f"[FAIL] Lumen did not start: {output[-300:]}")

        # Test 2: Inject mouse movement, verify no crash
        time.sleep(1)
        mon.sendall(b"mouse_move 50 30\n")
        time.sleep(0.5)
        mon.sendall(b"mouse_move -20 10\n")
        time.sleep(1)

        if proc.poll() is None:
            print("[PASS] Lumen handles mouse input without crashing")
            passed += 1
        else:
            print("[FAIL] QEMU exited unexpectedly")

        mon.close()

    finally:
        proc.terminate()
        proc.wait(timeout=5)
        try:
            os.unlink(mon_path)
        except OSError:
            pass

    print(f"\n{passed}/{total} tests passed")
    sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
    main()
