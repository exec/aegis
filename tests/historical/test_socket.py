#!/usr/bin/env python3
"""test_socket.py — Phase 26 socket API integration test.
Boots Aegis with q35 + virtio-net + NVMe, uses static IP 10.0.2.15 (Phase 25),
launches httpd via vigil service, verifies HTTP response via host port forward."""

import subprocess, time, http.client, sys, os, select, fcntl, socket, threading

QEMU         = "qemu-system-x86_64"
ISO          = "build/aegis.iso"
DISK         = "build/disk.img"
BOOT_TIMEOUT = 120
HOST_PORT    = 18080

def _read_until(proc, deadline, needle):
    buf = b""
    while time.time() < deadline:
        ready, _, _ = select.select([proc.stdout], [], [], 0.1)
        if ready:
            try:
                chunk = proc.stdout.read(4096)
                if chunk:
                    buf += chunk
                    sys.stdout.write(chunk.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
                if needle.encode() in buf:
                    return buf.decode("utf-8", errors="replace")
            except BlockingIOError:
                pass
        if proc.poll() is not None:
            break
    return buf.decode("utf-8", errors="replace")

def wait_port_free(port, timeout=30):
    """Wait until port is not in use (lingering QEMU from previous run)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("", port))
            s.close()
            return True
        except OSError:
            time.sleep(0.5)
    return False

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

    # Wait for port 18080 to be free (lingering QEMU from previous test run)
    if not wait_port_free(HOST_PORT):
        print(f"FAIL: port {HOST_PORT} still in use after 30s — kill lingering qemu")
        sys.exit(1)

    proc = subprocess.Popen(
        [QEMU,
         "-machine", "q35", "-cpu", "Broadwell",
         "-cdrom", ISO, "-boot", "order=d",
         "-display", "none", "-vga", "std", "-nodefaults",
         "-serial", "stdio", "-no-reboot", "-m", "2G",
         "-drive", f"file={DISK},if=none,id=nvme0",
         "-device", "nvme,drive=nvme0,serial=aegis0",
         "-device", "virtio-net-pci,netdev=n0,disable-legacy=on",
         "-netdev", f"user,id=n0,hostfwd=tcp::{HOST_PORT}-:80"],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    flags = fcntl.fcntl(proc.stdout.fileno(), fcntl.F_GETFL)
    fcntl.fcntl(proc.stdout.fileno(), fcntl.F_SETFL, flags | os.O_NONBLOCK)

    try:
        # Wait for vigil init capabilities to confirm kernel is up
        print("  waiting for vigil init caps...")
        out = _read_until(proc, time.time() + BOOT_TIMEOUT, "[CAP] OK:")
        if "[CAP] OK:" not in out:
            print("FAIL: vigil did not receive init capabilities within timeout")
            proc.kill(); proc.wait()
            sys.exit(1)
        print("  vigil started as init (caps granted)")

        # Wait for DHCP to acquire an IP — unicast packets are dropped until
        # s_my_ip is set, so HTTP requests fail before DHCP completes.
        print("  waiting for DHCP...")
        out_dhcp = _read_until(proc, time.time() + 60, "acquired")
        if "acquired" not in out_dhcp:
            print("WARN: DHCP not confirmed, continuing anyway")

        # Wait for httpd to be ready
        print("  waiting for httpd...")
        out2 = _read_until(proc, time.time() + 30, "httpd: waiting for connection")
        sys.stdout.write(out2)
        sys.stdout.flush()

        # Keep draining serial output in the background so QEMU's stdout pipe
        # never fills up and blocks QEMU's main loop (which would freeze SLIRP).
        def _bg_drain():
            while proc.poll() is None:
                ready, _, _ = select.select([proc.stdout], [], [], 0.2)
                if ready:
                    try:
                        proc.stdout.read(65536)
                    except (BlockingIOError, OSError):
                        pass
        threading.Thread(target=_bg_drain, daemon=True).start()

        # Try HTTP request via forwarded port
        for attempt in range(20):
            try:
                conn = http.client.HTTPConnection("localhost", HOST_PORT, timeout=10)
                conn.request("GET", "/")
                resp = conn.getresponse()
                body = resp.read().decode()
                conn.close()
                if resp.status == 200 and "Aegis" in body:
                    print(f"PASS: HTTP {resp.status}, body: {body.strip()!r}")
                    proc.kill(); proc.wait()
                    sys.exit(0)
                print(f"  Attempt {attempt+1}: status={resp.status} body={body.strip()!r}")
            except Exception as e:
                print(f"  Attempt {attempt+1}: {e}")
            time.sleep(1)

        print("FAIL: no valid HTTP response after 20 attempts")
        sys.exit(1)

    finally:
        proc.kill()
        stderr_out = proc.stderr.read() if proc.stderr else b""
        proc.wait()
        if stderr_out:
            sys.stdout.write("[QEMU stderr]: " + stderr_out.decode("utf-8", errors="replace")[:2000] + "\n")

if __name__ == "__main__":
    run_test()
