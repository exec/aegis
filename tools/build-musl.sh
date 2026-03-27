#!/bin/bash
# build-musl.sh — Build musl as a shared library for dynamic linking.
#
# Output:
#   build/musl-dynamic/lib/libc.so                — shared library + interpreter
#   build/musl-dynamic/lib/ld-musl-x86_64.so.1    — symlink to libc.so
#   build/musl-dynamic/usr/bin/musl-gcc            — gcc wrapper for dynamic linking
#   build/musl-dynamic/usr/lib/musl-gcc.specs      — gcc specs file
#
# Must run on Linux (musl builds for the host architecture).

set -euo pipefail

MUSL_VER=1.2.5
MUSL_URL="https://musl.libc.org/releases/musl-${MUSL_VER}.tar.gz"
MUSL_TAR="references/musl-${MUSL_VER}.tar.gz"
MUSL_SRC="references/musl-${MUSL_VER}"
DESTDIR="$(pwd)/build/musl-dynamic"

# Skip if already built
if [ -f "${DESTDIR}/lib/libc.so" ]; then
    echo "[build-musl] libc.so already exists, skipping."
    exit 0
fi

# Download if absent
if [ ! -d "${MUSL_SRC}" ]; then
    mkdir -p references
    if [ ! -f "${MUSL_TAR}" ]; then
        echo "[build-musl] Downloading musl ${MUSL_VER}..."
        curl -L -o "${MUSL_TAR}" "${MUSL_URL}"
    fi
    echo "[build-musl] Extracting..."
    tar -xzf "${MUSL_TAR}" -C references/
fi

# Configure
cd "${MUSL_SRC}"
if [ ! -f config.mak ]; then
    echo "[build-musl] Configuring..."
    ./configure \
        --prefix=/usr \
        --syslibdir=/lib \
        --enable-shared \
        CFLAGS="-O2 -fno-pie"
fi

# Build
echo "[build-musl] Building..."
make -j"$(nproc)"

# Install to DESTDIR
echo "[build-musl] Installing to ${DESTDIR}..."
make install DESTDIR="${DESTDIR}"

echo "[build-musl] Done. libc.so at ${DESTDIR}/lib/libc.so"
