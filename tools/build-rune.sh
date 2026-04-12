#!/bin/bash
# build-rune.sh — Build rune text editor as a static x86-64 binary.
#
# Output: user/bin/rune (ET_EXEC, statically linked, musl)
#
# Must run on Linux with Rust + musl target installed.
# Install musl target: rustup target add x86_64-unknown-linux-musl

set -euo pipefail

RUNE_REPO="https://github.com/exec/rune.git"
RUNE_SRC="references/rune"
RUNE_BIN="user/bin/rune"

# Skip if already built
if [ -f "$RUNE_BIN" ]; then
    echo "[rune] binary already exists, skipping."
    exit 0
fi

# Clone or update
if [ -d "$RUNE_SRC" ]; then
    echo "[rune] updating existing checkout..."
    git -C "$RUNE_SRC" pull --ff-only || true
else
    echo "[rune] cloning from $RUNE_REPO..."
    git clone "$RUNE_REPO" "$RUNE_SRC"
fi

# Ensure musl target is installed
rustup target add x86_64-unknown-linux-musl 2>/dev/null || true

# Build as non-PIE static binary (ET_EXEC for Aegis ELF loader compatibility)
echo "[rune] building for x86_64-unknown-linux-musl (static, non-PIE)..."
cd "$RUNE_SRC"
RUSTFLAGS="-C target-feature=+crt-static -C relocation-model=static" \
    cargo build --release --target x86_64-unknown-linux-musl

cd - > /dev/null
cp "$RUNE_SRC/target/x86_64-unknown-linux-musl/release/rune" "$RUNE_BIN"
echo "[rune] built: $RUNE_BIN ($(du -h "$RUNE_BIN" | cut -f1))"
