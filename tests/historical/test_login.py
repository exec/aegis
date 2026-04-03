#!/usr/bin/env python3
"""test_login.py — Integration test for login binary on Aegis.

Requires build/disk.img (run make disk first).
Boots INIT=login on q35 + NVMe, sends credentials via QEMU monitor keyboard
injection, verifies oksh prompt appears after successful auth.
Also tests wrong-password delay path.
"""
import subprocess, time, sys, os, socket, select, tempfile, fcntl

QEMU         = "qemu-system-x86_64"
ISO          = "build/aegis.iso"
DISK         = "build/disk.img"
BOOT_TIMEOUT = 120
CMD_TIMEOUT  = 30

_KEY_MAP = {
    ' ': 'spc', '\n': 'ret',
    '/': 'slash', '-': 'minus', '.': 'dot',
}
for c in 'abcdefghijklmnopqrstuvwxyz': _KEY_MAP[c] = c
for c in '0123456789': _KEY_MAP.setdefault(c, c)

def _type_string(mon_sock, s):
    for ch in s:
        key = _KEY_MAP.get(ch)
        if key is None: continue
        cmd = f'sendkey {key}\n'.encode()
        mon_sock.sendall(cmd)
        time.sleep(0.08)
        try: mon_sock.recv(4096)
        except: pass

def _read_until(proc, deadline, needle):
    buf = b""
    while time.time() < deadline:
        ready, _, _ = select.select([proc.stdout], [], [], 0.1)
        if ready:
            try:
                chunk = proc.stdout.read(4096)
                if chunk: buf += chunk
                if needle.encode() in buf: return buf.decode("utf-8", errors="replace")
            except BlockingIOError: pass
        if proc.poll() is not None: break
    return buf.decode("utf-8", errors="replace")

def build_iso():
    # Run via bash so grub-mkrescue inherits a clean shell environment and
    # avoids stale root-owned /tmp/grub.* directories from previous sudo builds.
    r = subprocess.run("make INIT=login iso", shell=True, capture_output=True)
    if r.returncode != 0:
        print("[FAIL] make INIT=login iso failed")
        print(r.stderr.decode())
        sys.exit(1)

def run_test():
    if not os.path.exists(DISK):
        print(f"SKIP: {DISK} not found — run 'make disk' first")
        sys.exit(0)

    build_iso()

    mon_path = tempfile.mktemp(suffix=".sock")
    proc = subprocess.Popen(
        [QEMU,
         "-machine", "q35", "-cpu", "Broadwell",
         "-cdrom", ISO, "-boot", "order=d",
         "-display", "none", "-vga", "std", "-nodefaults",
         "-serial", "stdio", "-no-reboot", "-m", "2G",
         "-drive", f"file={DISK},if=none,id=nvme0",
         "-device", "nvme,drive=nvme0,serial=aegis0",
         "-monitor", f"unix:{mon_path},server,nowait"],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    flags = fcntl.fcntl(proc.stdout.fileno(), fcntl.F_GETFL)
    fcntl.fcntl(proc.stdout.fileno(), fcntl.F_SETFL, flags | os.O_NONBLOCK)

    # Wait for monitor socket
    deadline = time.time() + 10
    while not os.path.exists(mon_path) and time.time() < deadline:
        time.sleep(0.1)

    mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    mon.connect(mon_path)
    mon.setblocking(False)
    try: mon.recv(4096)
    except: pass

    all_out = ""
    errors = []

    try:
        # ── Wait for login prompt ─────────────────────────────────────────
        print("  waiting for 'login: ' prompt...")
        out = _read_until(proc, time.time() + BOOT_TIMEOUT, "login: ")
        all_out += out
        if "login: " not in out:
            errors.append("FAIL: 'login: ' prompt not seen within timeout")
            proc.kill(); proc.wait()
            for e in errors: print(e)
            sys.exit(1)
        print("  login prompt found")

        # ── Test wrong password (fast path — 3 s delay) ───────────────────
        print("  testing wrong password...")
        _type_string(mon, "root\n")
        out2 = _read_until(proc, time.time() + 10, "password: ")
        all_out += out2
        if "password: " not in out2:
            errors.append("FAIL: 'password: ' not seen after username")

        t0 = time.time()
        _type_string(mon, "wrongpassword\n")
        out3 = _read_until(proc, time.time() + 10, "Login incorrect")
        all_out += out3
        elapsed = time.time() - t0
        if "Login incorrect" not in out3:
            errors.append("FAIL: 'Login incorrect' not seen after wrong password")
        if elapsed < 2.5:
            errors.append(f"FAIL: no 3-second delay on wrong password (got {elapsed:.1f}s)")
        else:
            print(f"  wrong-password delay OK ({elapsed:.1f}s)")

        # ── Wait for re-prompt and send correct credentials ───────────────
        out4 = _read_until(proc, time.time() + 5, "login: ")
        all_out += out4

        print("  sending correct credentials...")
        _type_string(mon, "root\n")
        out5 = _read_until(proc, time.time() + 10, "password: ")
        all_out += out5

        _type_string(mon, "forevervigilant\n")
        out6 = _read_until(proc, time.time() + CMD_TIMEOUT, "root@aegis")
        all_out += out6

        if "root@aegis" not in out6 and "#" not in out6:
            errors.append("FAIL: oksh prompt not seen after correct login")
        else:
            print("  oksh prompt found after login")

    finally:
        proc.kill()
        proc.wait()
        mon.close()
        try: os.unlink(mon_path)
        except: pass

    if errors:
        for e in errors: print(e)
        sys.exit(1)

    print("PASS test_login")
    sys.exit(0)

if __name__ == "__main__":
    run_test()
