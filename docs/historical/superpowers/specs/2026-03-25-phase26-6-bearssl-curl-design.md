# Phase 26.6: BearSSL + curl Binary

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Compile BearSSL as a static library and curl as a musl-linked binary with HTTPS support, place them on the ext2 partition, and verify `curl https://example.com` works from the Aegis shell.

**Architecture:** BearSSL 0.6 is compiled with `musl-gcc` into a static archive (`libbearssl.a`) installed into a staging directory. curl 8.13.0 (source already in `references/curl/`) is configured with `--with-bearssl=<staging-dir>` (autoconf path detection), `--without-openssl`, built against musl, then stripped and placed at `/bin/curl` on the ext2 image.

**Tech Stack:** BearSSL 0.6, curl 8.13.0, musl-gcc, autoconf build, ext2 image.

**Prerequisites:** Phase 26 socket API (TCP/UDP sockets, including `SO_RCVTIMEO` and `SO_SNDTIMEO` in `setsockopt`), Phase 28 DHCP (IP address + `/etc/resolv.conf`), DNS via musl stub resolver.

---

## Constraints and Non-Negotiables

- BearSSL only. curl must be configured `--without-openssl --without-mbedtls --without-wolfssl --with-bearssl=<staging>`.
- Static linking only. `curl` is a single self-contained binary, linked with `musl-gcc -static`.
- The curl binary must live on the ext2 partition at `/bin/curl` — **not** in the initrd (too large). ext2 disk is required.
- `make test` is unaffected. The boot test uses `-machine pc` with no disk. curl is simply absent.
- BearSSL 0.6 source tarball is downloaded to `references/bearssl-0.6/` (tarball extracts to a `bearssl-0.6/` directory). Source is **not** yet in the repo — it must be fetched as part of this phase.
- curl source is already in `references/curl/` (flat, no version subdirectory). A `configure` script is present.
- curl's build system is autoconf. Use `./configure`; do not use CMake.
- CA bundle: curl needs a Mozilla CA bundle compiled in. Download once to `tools/cacert.pem` and commit. Place at `/etc/ssl/certs/ca-certificates.crt` on the ext2 image. Compile the path in via `--with-ca-bundle=/etc/ssl/certs/ca-certificates.crt`.
- DNS: musl's stub resolver reads `/etc/resolv.conf`. After DHCP, this file contains `nameserver 10.0.2.3`. curl uses musl's `getaddrinfo` → UDP socket → DNS query. No kernel-internal DNS. **Phase 26 must implement `SO_RCVTIMEO` in `setsockopt`** or musl's resolver will hang indefinitely on a non-responsive DNS server.

---

## File Layout

| File | Action | Purpose |
|------|--------|---------|
| `references/bearssl-0.6/` | Create | BearSSL 0.6 source (fetched by `tools/fetch-bearssl.sh`) |
| `build/bearssl-install/` | Generated | BearSSL staging install (`include/` + `lib/libbearssl.a`) |
| `build/curl-build/` | Generated | VPATH build directory (keep source tree clean) |
| `build/curl/curl` | Generated | Stripped curl binary |
| `tools/fetch-bearssl.sh` | Create | Downloads BearSSL 0.6 tarball and extracts to `references/bearssl-0.6/` |
| `tools/build-bearssl.sh` | Create | Compiles BearSSL with musl-gcc; installs headers + archive to `build/bearssl-install/` |
| `tools/build-curl.sh` | Create | Configures and compiles curl with `--with-bearssl=build/bearssl-install`; outputs `build/curl/curl` |
| `tools/cacert.pem` | Create + Commit | Mozilla CA bundle (~220 KB); committed for reproducible offline builds |
| `Makefile` | Modify | `bearssl_install`, `curl_bin` targets; extend `disk` target with curl + CA bundle |

---

## BearSSL Build

BearSSL 0.6 source: `https://bearssl.org/bearssl-0.6.tar.gz`

BearSSL's `src/` directory has multiple subdirectories (`src/ssl/`, `src/symcipher/`, `src/hash/`, etc.). The `*.c` glob does not match recursively — use `find` instead.

```bash
#!/bin/bash
# tools/build-bearssl.sh
set -e

BEARSSL_SRC="$(git rev-parse --show-toplevel)/references/bearssl-0.6"
STAGING="$(git rev-parse --show-toplevel)/build/bearssl-install"
OBJDIR="$(git rev-parse --show-toplevel)/build/bearssl-objs"

# Create both lib/ and lib64/ so curl's configure finds libbearssl.a
# regardless of whether $libsuff is empty or "64" on the build host.
mkdir -p "$STAGING/include" "$STAGING/lib" "$STAGING/lib64" "$OBJDIR"
rm -f "$OBJDIR"/*.o   # clean any stale objects from prior partial build

# Compile all BearSSL sources (recursive — src has subdirectories).
# Use a flat mangled object name (<subdir>__<basename>.o) to prevent
# silent overwrites if any two source files share the same basename
# across different subdirectories.
while IFS= read -r -d '' src; do
    rel="${src#$BEARSSL_SRC/src/}"   # e.g. "ssl/ssl_rec_cbc.c"
    obj="$OBJDIR/$(echo "$rel" | tr '/' '__' | sed 's/\.c$/.o/')"
    musl-gcc -O2 -I"$BEARSSL_SRC/inc" -c "$src" -o "$obj"
done < <(find "$BEARSSL_SRC/src" -name '*.c' -print0)

# Guard: fail loudly if no objects were produced.
# Use -f check on the first element — a failed glob expands to a literal
# "*.o" string (1-element array) not an empty array, so length check is wrong.
OBJS=( "$OBJDIR"/*.o )
[ -f "${OBJS[0]}" ] || { echo "[bearssl] ERROR: no objects compiled — check BEARSSL_SRC path"; exit 1; }

# Create static archive
ar rcs "$STAGING/lib/libbearssl.a" "${OBJS[@]}"
# Mirror into lib64/ so configure finds it on multilib hosts
cp "$STAGING/lib/libbearssl.a" "$STAGING/lib64/libbearssl.a"

# Install headers
cp -r "$BEARSSL_SRC/inc/." "$STAGING/include/"

echo "[bearssl] built: $STAGING/lib/libbearssl.a"
```

---

## curl Configuration

curl source is in `references/curl/` (flat directory, `configure` script present).

curl's autoconf BearSSL detection (`m4/curl-bearssl.m4`) expects `--with-bearssl=PATH` pointing to an installation root containing `include/bearssl.h` and `lib/libbearssl.a`. It constructs `-I$PATH/include` and `-L$PATH/lib` itself. **Do not pass `BEARSSL_CFLAGS` or `BEARSSL_LIBS` env vars — they are ignored by curl's configure.**

**Important:** Configure must run in a **separate build directory** (not in `references/curl/` directly). Running in-source pollutes the committed source tree with build artifacts and causes stale-object failures on second-pass builds.

```bash
#!/bin/bash
# tools/build-curl.sh
set -e

REPO="$(git rev-parse --show-toplevel)"
BEARSSL_INSTALL="$REPO/build/bearssl-install"
CURL_SRC="$REPO/references/curl"
BUILD_DIR="$REPO/build/curl-build"
OUT="$REPO/build/curl"

mkdir -p "$BUILD_DIR" "$OUT"

# Run configure from a separate build directory (VPATH build)
cd "$BUILD_DIR"

"$CURL_SRC/configure" \
  CC="musl-gcc" \
  CFLAGS="-O2" \
  LDFLAGS="-static" \
  PKG_CONFIG="" \
  --srcdir="$CURL_SRC" \
  --prefix="$OUT/install" \
  --host=x86_64-linux-musl \
  --disable-shared \
  --enable-static \
  --without-openssl \
  --without-mbedtls \
  --without-wolfssl \
  --without-libpsl \
  --without-libidn2 \
  --without-nghttp2 \
  --without-nghttp3 \
  --without-zlib \
  --without-brotli \
  --without-zstd \
  --disable-ldap \
  --disable-ldaps \
  --disable-rtsp \
  --disable-pop3 \
  --disable-imap \
  --disable-smtp \
  --disable-telnet \
  --disable-dict \
  --disable-tftp \
  --disable-gopher \
  --disable-mqtt \
  --with-bearssl="$BEARSSL_INSTALL" \
  --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt

make -j$(nproc)

# Copy and strip the binary (built at src/curl relative to build dir)
cp src/curl "$OUT/curl"
strip "$OUT/curl"

echo "[curl] built: $OUT/curl ($(du -sh "$OUT/curl" | cut -f1))"
```

**Note:** `--with-default-ssl-backend=bearssl` is not needed when BearSSL is the only backend. It is harmless but generates a configure warning.

**If configure fails to detect BearSSL** (e.g., on a multilib host where `$libsuff=64` makes it look in `lib64/` exclusively): add an explicit `LDFLAGS` override to the configure invocation:

```bash
  LDFLAGS="-static -L$BEARSSL_INSTALL/lib -L$BEARSSL_INSTALL/lib64" \
```

This forces both search paths and overrides whatever `$libsuff` detection resolves to.

### Expected binary size

BearSSL adds ~40 KB of TLS code. A minimal static curl with no compression, no HTTP/2, and BearSSL typically compiles to ~2–3 MB stripped. This is acceptable for ext2 (partition is 64 MB).

---

## Makefile Targets

```makefile
# Fetch BearSSL source (only if not present)
references/bearssl-0.6/inc/bearssl.h:
	bash tools/fetch-bearssl.sh

# BearSSL staging install
build/bearssl-install/lib/libbearssl.a: references/bearssl-0.6/inc/bearssl.h
	bash tools/build-bearssl.sh

# curl binary
build/curl/curl: build/bearssl-install/lib/libbearssl.a
	bash tools/build-curl.sh

# disk target: extend to include curl + CA bundle
# In the e2tools population section:
#   e2mkdir aegis_disk.img:/etc/ssl
#   e2mkdir aegis_disk.img:/etc/ssl/certs
#   e2cp tools/cacert.pem aegis_disk.img:/etc/ssl/certs/ca-certificates.crt
#   e2cp build/curl/curl aegis_disk.img:/bin/curl
```

---

## BearSSL Fetch Script

```bash
#!/bin/bash
# tools/fetch-bearssl.sh
set -e
REPO="$(git rev-parse --show-toplevel)"
URL="https://bearssl.org/bearssl-0.6.tar.gz"
TMP="/tmp/bearssl-0.6.tar.gz"

wget -O "$TMP" "$URL"
cd "$REPO/references"
tar xzf "$TMP"
echo "[bearssl] extracted to references/bearssl-0.6/"
```

---

## CA Bundle

Download once to `tools/cacert.pem` and commit to the repository (offline reproducibility):

```bash
wget -O tools/cacert.pem https://curl.se/ca/cacert.pem
# ~220 KB
git add tools/cacert.pem
git commit -m "tools: add Mozilla CA bundle for curl HTTPS"
```

---

## DNS Resolution Path

musl's `getaddrinfo` reads `/etc/resolv.conf` and sends a UDP DNS query to the listed nameserver. After DHCP:

```
/etc/resolv.conf:
  nameserver 10.0.2.3
```

QEMU SLIRP's built-in DNS server at `10.0.2.3` resolves public hostnames by forwarding to the host. musl's resolver sets **both `SO_RCVTIMEO` and `SO_SNDTIMEO`** (5 seconds) on the UDP socket — **Phase 26 must implement both options in `setsockopt`** or DNS queries hang indefinitely when the server does not respond. If `setsockopt` returns `EINVAL` for either option, musl will proceed without a timeout (functionally degraded but not fatal on a cooperative QEMU SLIRP server).

The full path for `curl https://example.com`:
1. musl `getaddrinfo("example.com")` → UDP socket (SO_RCVTIMEO=5s) → DNS query to 10.0.2.3
2. QEMU SLIRP forwards → host DNS → response
3. curl opens TCP socket to port 443
4. BearSSL TLS handshake (validates against `/etc/ssl/certs/ca-certificates.crt`)
5. HTTP/1.1 GET, response printed to stdout

---

## Test Plan

`tests/test_curl.py` (new, added to `tests/run_tests.sh`):

```python
# Boots q35 + virtio-net + disk image
# Waits for [DHCP] lease in serial output
# Sends: curl -s https://example.com | head -5
# Expects: <!doctype html> (or similar HTML) within 15 seconds

assert "<!doctype html>" in output.lower()
```

QEMU user networking supports HTTPS via the host network. `example.com` is a stable IANA-hosted domain appropriate for integration tests.

---

## Forward-Looking Constraints

- **HTTP/2 not supported.** `--without-nghttp2` disables HTTP/2. curl falls back to HTTP/1.1. HTTP/2 requires `nghttp2` library — Phase 29+.
- **No compression.** `--without-zlib --without-brotli --without-zstd` disables content-encoding. Servers sending compressed responses will produce garbled output. `Accept-Encoding` header is not sent.
- **No IPv6.** Aegis network stack is IPv4 only.
- **No cookie persistence.** Stateless HTTP only.
- **ext2 required.** A system booted from ISO without ext2 disk has no curl binary.
- **BearSSL source pinned to 0.6.** Once fetched and verified, commit the extracted source to `references/bearssl-0.6/` for reproducible offline builds (do not rely solely on the tarball URL).
- **`sys_netcfg` is Aegis-specific.** Standard tools expect `ioctl(SIOCSIFADDR)` / netlink. Future phase may add rtnetlink for compatibility.
