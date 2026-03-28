#!/usr/bin/env python3
"""test_installer.py — Two-boot installation test.

Boot 1: Live ISO + empty NVMe → run installer → shutdown
Boot 2: NVMe only (no ISO) → verify boot + login
"""
import subprocess, sys, os, select, fcntl, time, socket, tempfile

QEMU = "qemu-system-x86_64"
ISO  = "build/aegis.iso"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOOT_TIMEOUT = 120
CMD_TIMEOUT  = 60

_KEY_MAP = {
    ' ':  'spc', '\n': 'ret', '/':  'slash', '-':  'minus', '.':  'dot',
    ':':  'shift-semicolon', '|':  'shift-backslash', '_':  'shift-minus',
    '<':  'shift-comma', '>':  'shift-dot', '&':  'shift-7',
    '=': 'equal', '(': 'shift-9', ')': 'shift-0', '"': 'shift-apostrophe',
    '!': 'shift-1', '?': 'shift-slash', ',': 'comma',
}
for c in 'abcdefghijklmnopqrstuvwxyz':
    _KEY_MAP[c] = c
for c in '0123456789':
    _KEY_MAP.setdefault(c, c)

def _type_string(mon_sock, s):
    for ch in s:
        key = _KEY_MAP.get(ch)
        if key is None: continue
        mon_sock.sendall(f'sendkey {key}\n'.encode())
        time.sleep(0.08)
        try: mon_sock.recv(4096)
        except OSError: pass

def _drain(fd, timeout=0.5):
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.2)
        if ready:
            try:
                chunk = os.read(fd, 65536)
                if chunk: buf += chunk
            except (BlockingIOError, OSError): pass
    return buf

def _wait_for(fd, needle, timeout):
    enc = needle.encode() if isinstance(needle, str) else needle
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.5)
        if ready:
            try:
                chunk = os.read(fd, 65536)
                if chunk: buf += chunk
            except (BlockingIOError, OSError): pass
        if enc in buf:
            return True, buf
    return False, buf

def run_test():
    iso_path = os.path.join(ROOT, ISO)
    if not os.path.exists(iso_path):
        print("SKIP: ISO not found")
        sys.exit(0)

    # Create empty 128MB NVMe disk
    fd_disk, disk_path = tempfile.mkstemp(suffix=".img")
    os.close(fd_disk)
    subprocess.run(["dd", "if=/dev/zero", f"of={disk_path}", "bs=1M", "count=128"],
                   capture_output=True)

    mon_path = tempfile.mktemp(suffix=".sock")

    try:
        # ── Boot 1: Live ISO + empty NVMe ──
        print("Boot 1: Live ISO + installer...")
        proc = subprocess.Popen([
            QEMU, "-machine", "q35", "-cpu", "Broadwell",
            "-cdrom", iso_path, "-boot", "order=d",
            "-display", "none", "-vga", "std", "-nodefaults",
            "-serial", "stdio", "-no-reboot", "-m", "2G",
            "-drive", f"file={disk_path},if=none,id=nvme0,format=raw",
            "-device", "nvme,drive=nvme0,serial=aegis0",
            "-device", "virtio-net-pci,netdev=n0,disable-legacy=on",
            "-netdev", "user,id=n0",
            "-monitor", f"unix:{mon_path},server,nowait"],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        flags = fcntl.fcntl(proc.stdout.fileno(), fcntl.F_GETFL)
        fcntl.fcntl(proc.stdout.fileno(), fcntl.F_SETFL, flags | os.O_NONBLOCK)

        # Wait for monitor socket
        deadline = time.time() + 10
        while not os.path.exists(mon_path) and time.time() < deadline:
            time.sleep(0.1)
        mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        mon.connect(mon_path)
        mon.setblocking(False)

        # Login
        ok, _ = _wait_for(proc.stdout.fileno(), "login: ", BOOT_TIMEOUT)
        if not ok:
            print("FAIL: login prompt not found in boot 1")
            sys.exit(1)
        _type_string(mon, "root\n")
        ok, _ = _wait_for(proc.stdout.fileno(), "assword", 10)
        if not ok:
            print("FAIL: password prompt not found in boot 1")
            sys.exit(1)
        _type_string(mon, "forevervigilant\n")
        ok, _ = _wait_for(proc.stdout.fileno(), "# ", 10)
        if not ok:
            print("FAIL: shell prompt not found in boot 1")
            sys.exit(1)

        # Run installer, answer prompts
        time.sleep(1)
        _type_string(mon, "installer\n")
        ok, _ = _wait_for(proc.stdout.fileno(), "[y/N]", CMD_TIMEOUT)
        if not ok:
            print("FAIL: installer disk prompt not found")
            sys.exit(1)
        _type_string(mon, "y\n")

        # Answer user setup prompts
        ok, _ = _wait_for(proc.stdout.fileno(), "Username", 10)
        if ok:
            _type_string(mon, "\n")  # accept default (root)
        ok, _ = _wait_for(proc.stdout.fileno(), "Password:", 10)
        if ok:
            _type_string(mon, "testpass\n")
        ok, _ = _wait_for(proc.stdout.fileno(), "Confirm", 10)
        if ok:
            _type_string(mon, "testpass\n")

        # Wait for installation to complete
        ok, buf = _wait_for(proc.stdout.fileno(), "Installation complete", CMD_TIMEOUT)
        if not ok:
            print("FAIL: installation did not complete")
            print("  tail:", buf[-500:].decode("utf-8", errors="replace"))
            sys.exit(1)
        print("  Installation complete in boot 1")

        # Shutdown
        time.sleep(1)
        _type_string(mon, "exit\n")
        time.sleep(3)
        try: mon.close()
        except OSError: pass
        proc.kill()
        proc.wait()
        try: os.unlink(mon_path)
        except OSError: pass

        # ── Boot 2: UEFI standalone NVMe (no ISO) ──
        # Use OVMF (UEFI firmware) to boot directly from the installed NVMe.
        # OVMF has NVMe drivers and can load BOOTX64.EFI from the ESP.
        OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
        OVMF_VARS_SRC = "/usr/share/OVMF/OVMF_VARS_4M.fd"
        print("Boot 2: OVMF + NVMe only (true standalone UEFI boot)...")
        # Copy OVMF NVRAM — OVMF needs writable NVRAM to create boot entries
        fd_vars, vars_path = tempfile.mkstemp(suffix=".fd")
        os.close(fd_vars)
        import shutil
        shutil.copy2(OVMF_VARS_SRC, vars_path)
        mon_path2 = tempfile.mktemp(suffix=".sock")
        proc2 = subprocess.Popen([
            QEMU, "-machine", "q35", "-cpu", "Broadwell",
            "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_CODE}",
            "-drive", f"if=pflash,format=raw,file={vars_path}",
            "-display", "none", "-vga", "std", "-nodefaults",
            "-serial", "stdio", "-no-reboot", "-m", "2G",
            "-drive", f"file={disk_path},if=none,id=nvme0,format=raw",
            "-device", "nvme,drive=nvme0,serial=aegis0",
            "-monitor", f"unix:{mon_path2},server,nowait"],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        flags = fcntl.fcntl(proc2.stdout.fileno(), fcntl.F_GETFL)
        fcntl.fcntl(proc2.stdout.fileno(), fcntl.F_SETFL, flags | os.O_NONBLOCK)

        deadline = time.time() + 10
        while not os.path.exists(mon_path2) and time.time() < deadline:
            time.sleep(0.1)
        mon2 = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        mon2.connect(mon_path2)
        mon2.setblocking(False)

        # Wait for login prompt from installed system
        ok, buf = _wait_for(proc2.stdout.fileno(), "login: ", BOOT_TIMEOUT)
        if not ok:
            print("FAIL: login prompt not found in boot 2 (installed system)")
            print("  tail:", buf[-500:].decode("utf-8", errors="replace"))
            sys.exit(1)
        print("  Login prompt found on installed system")

        # Verify no ramdisk (UEFI standalone — no module loaded)
        if b"[RAMDISK]" in buf:
            print("FAIL: standalone boot should not have [RAMDISK]")
            sys.exit(1)
        print("  No [RAMDISK] in boot output (correct — standalone UEFI)")

        # Verify [EXT2] mounted from NVMe
        if b"[EXT2] OK: mounted nvme0p1" not in buf:
            print("FAIL: installed system should mount nvme0p1")
            print("  tail:", buf[-500:].decode("utf-8", errors="replace"))
            sys.exit(1)
        print("  [EXT2] OK: mounted nvme0p1 (correct)")

        try: mon2.close()
        except OSError: pass
        proc2.kill()
        proc2.wait()
        try: os.unlink(mon_path2)
        except OSError: pass

        print("PASS test_installer")

    finally:
        try: os.unlink(disk_path)
        except OSError: pass
        try: os.unlink(vars_path)
        except (OSError, NameError): pass

if __name__ == "__main__":
    run_test()
