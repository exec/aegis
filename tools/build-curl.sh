#!/bin/bash
# tools/build-curl.sh — configure and compile curl with BearSSL
set -e

REPO="$(git rev-parse --show-toplevel)"
BEARSSL_INSTALL="$REPO/build/bearssl-install"
CURL_SRC="$REPO/references/curl"
BUILD_DIR="$REPO/build/curl-build"
OUT="$REPO/build/curl"

mkdir -p "$BUILD_DIR" "$OUT"

cd "$BUILD_DIR"

"$CURL_SRC/configure" \
  CC="musl-gcc" \
  CFLAGS="-O2" \
  LDFLAGS="-static -L$BEARSSL_INSTALL/lib -L$BEARSSL_INSTALL/lib64" \
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
cp src/curl "$OUT/curl"
strip "$OUT/curl"
echo "[curl] built: $OUT/curl ($(du -sh "$OUT/curl" | cut -f1))"
