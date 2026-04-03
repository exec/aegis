#!/usr/bin/env python3
"""test_vigil.py — Integration test for Vigil init system on Aegis.

Requires build/disk.img (run make disk first).
Boots INIT=vigil on q35 + NVMe, verifies vigil starts as init and
launches the getty service which runs login.
"""
import subprocess, time, sys, os, select, fcntl

QEMU         = "qemu-system-x86_64"
ISO          = "build/aegis.iso"
DISK         = "build/disk.img"
BOOT_TIMEOUT = 120

def _read_until(proc, deadline, needle):
    buf = b""
    while time.time() < deadline:
        ready, _, _ = select.select([proc.stdout], [], [], 0.1)
        if ready:
            try:
                chunk = proc.stdout.read(4096)
                if chunk:
                    buf += chunk
                if needle.encode() in buf:
                    return buf.decode("utf-8", errors="replace")
            except BlockingIOError:
                pass
        if proc.poll() is not None:
            break
    return buf.decode("utf-8", errors="replace")

def build_iso():
    r = subprocess.run("make INIT=vigil iso", shell=True, capture_output=True)
    if r.returncode != 0:
        print("[FAIL] make INIT=vigil iso failed")
        print(r.stderr.decode())
        sys.exit(1)

def run_test():
    if not os.path.exists(DISK):
        print(f"SKIP: {DISK} not found — run 'make disk' first")
        sys.exit(0)

    build_iso()

    proc = subprocess.Popen(
        [QEMU,
         "-machine", "q35", "-cpu", "Broadwell",
         "-cdrom", ISO, "-boot", "order=d",
         "-display", "none", "-vga", "std", "-nodefaults",
         "-serial", "stdio", "-no-reboot", "-m", "2G",
         "-drive", f"file={DISK},if=none,id=nvme0",
         "-device", "nvme,drive=nvme0,serial=aegis0"],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    flags = fcntl.fcntl(proc.stdout.fileno(), fcntl.F_GETFL)
    fcntl.fcntl(proc.stdout.fileno(), fcntl.F_SETFL, flags | os.O_NONBLOCK)

    errors = []
    try:
        print("  waiting for vigil init caps...")
        out = _read_until(proc, time.time() + BOOT_TIMEOUT, "[CAP] OK: 9 capabilities")
        if "[CAP] OK: 9 capabilities" not in out:
            errors.append("FAIL: vigil did not receive init capabilities within timeout")
            proc.kill(); proc.wait()
            for e in errors: print(e)
            sys.exit(1)
        print("  vigil started as init (caps granted)")

        print("  waiting for login prompt from getty service...")
        out2 = _read_until(proc, time.time() + 60, "login: ")
        if "login: " not in out2:
            errors.append("FAIL: login prompt not seen — getty service may not have started")
        else:
            print("  getty/login launched by vigil OK")

    finally:
        proc.kill()
        proc.wait()

    if errors:
        for e in errors: print(e)
        sys.exit(1)

    print("PASS test_vigil")
    sys.exit(0)

if __name__ == "__main__":
    run_test()
