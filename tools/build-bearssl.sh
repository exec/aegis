#!/bin/bash
# tools/build-bearssl.sh — compile BearSSL 0.6 with musl-gcc
set -e

BEARSSL_SRC="$(git rev-parse --show-toplevel)/references/bearssl-0.6"
STAGING="$(git rev-parse --show-toplevel)/build/bearssl-install"
OBJDIR="$(git rev-parse --show-toplevel)/build/bearssl-objs"

mkdir -p "$STAGING/include" "$STAGING/lib" "$STAGING/lib64" "$OBJDIR"
rm -f "$OBJDIR"/*.o

while IFS= read -r -d '' src; do
    rel="${src#$BEARSSL_SRC/src/}"
    obj="$OBJDIR/$(echo "$rel" | tr '/' '__' | sed 's/\.c$/.o/')"
    musl-gcc -O2 -I"$BEARSSL_SRC/inc" -I"$BEARSSL_SRC/src" -c "$src" -o "$obj"
done < <(find "$BEARSSL_SRC/src" -name '*.c' -print0)

OBJS=( "$OBJDIR"/*.o )
[ -f "${OBJS[0]}" ] || { echo "[bearssl] ERROR: no objects compiled"; exit 1; }

ar rcs "$STAGING/lib/libbearssl.a" "${OBJS[@]}"
cp "$STAGING/lib/libbearssl.a" "$STAGING/lib64/libbearssl.a"
cp -r "$BEARSSL_SRC/inc/." "$STAGING/include/"

echo "[bearssl] built: $STAGING/lib/libbearssl.a"
