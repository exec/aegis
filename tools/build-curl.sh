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
  CFLAGS="-O2 -fno-pie" \
  LDFLAGS="-static -no-pie -L$BEARSSL_INSTALL/lib -L$BEARSSL_INSTALL/lib64" \
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
  --disable-threaded-resolver \
  --with-bearssl="$BEARSSL_INSTALL" \
  --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt

make -j$(nproc) LDFLAGS="-static -no-pie -L$BEARSSL_INSTALL/lib -L$BEARSSL_INSTALL/lib64" V=1 2>&1 | tail -5

# Libtool often ignores -static. Force a static relink if needed.
if file src/curl | grep -q "dynamically linked"; then
    echo "[curl] libtool produced dynamic binary, relinking statically..."
    musl-gcc -static -no-pie -O2 -fno-pie -o src/curl \
        src/curl-tool_main.o src/.libs/libcurltool.a lib/.libs/libcurl.a \
        -L"$BEARSSL_INSTALL/lib" -L"$BEARSSL_INSTALL/lib64" -lbearssl
fi

cp src/curl "$OUT/curl"
strip "$OUT/curl"
echo "[curl] built: $OUT/curl ($(du -sh "$OUT/curl" | cut -f1))"
