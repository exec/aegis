#!/bin/bash
#
# build-rootfs.sh — Build the Aegis root filesystem ext2 image
#
# Reads rootfs.manifest for binary mappings and copies the rootfs/ skeleton.
# Single source of truth: add a binary to rootfs.manifest, done.
#
set -euo pipefail

ROOTFS_IMG="${1:?Usage: build-rootfs.sh <output.img> <kernel.elf> [wallpaper.raw] [logo.raw] [claude.raw]}"
KERNEL_ELF="${2:?}"
WALLPAPER_RAW="${3:-}"
LOGO_RAW="${4:-}"
CLAUDE_RAW="${5:-}"

MANIFEST="rootfs.manifest"
SKELETON="rootfs"
P1_SECTORS=120832
DEBUGFS="/sbin/debugfs"

# ── Create empty ext2 image ──────────────────────────────────────────────────
rm -f "$ROOTFS_IMG"
dd if=/dev/zero of="$ROOTFS_IMG" bs=512 count=$P1_SECTORS 2>/dev/null
/sbin/mke2fs -t ext2 -F -L aegis-root "$ROOTFS_IMG" >/dev/null 2>&1

# ── Helper: batch debugfs commands ───────────────────────────────────────────
debugfs_run() {
    $DEBUGFS -w "$ROOTFS_IMG" <<< "$1" >/dev/null 2>&1
}

# Track which directories we've already created
declare -A CREATED_DIRS

ensure_dir() {
    local dir="$1"
    if [[ -n "${CREATED_DIRS[$dir]:-}" ]]; then
        return
    fi
    # Ensure parent exists first
    local parent
    parent="$(dirname "$dir")"
    if [[ "$parent" != "/" && "$parent" != "." ]]; then
        ensure_dir "$parent"
    fi
    debugfs_run "mkdir $dir"
    CREATED_DIRS["$dir"]=1
}

# ── Copy skeleton directory tree ─────────────────────────────────────────────
# Walk the rootfs/ skeleton and replicate its structure + files into the image.
echo "[rootfs] Copying skeleton..."

# First pass: create all directories
while IFS= read -r -d '' dir; do
    rel="${dir#$SKELETON}"
    [[ -z "$rel" ]] && continue
    ensure_dir "$rel"
done < <(find "$SKELETON" -type d -print0 | sort -z)

# Second pass: copy all files
while IFS= read -r -d '' file; do
    rel="${file#$SKELETON}"
    debugfs_run "write $file $rel"
done < <(find "$SKELETON" -type f -print0 | sort -z)

# ── Process manifest: copy binaries ─────────────────────────────────────────
echo "[rootfs] Installing binaries from manifest..."

CMDS=""
CHMOD_CMDS=""

while IFS= read -r line; do
    # Strip comments and blank lines
    line="${line%%#*}"
    line="$(echo "$line" | xargs)"
    [[ -z "$line" ]] && continue

    src=$(echo "$line" | awk '{print $1}')
    dest=$(echo "$line" | awk '{print $2}')

    if [[ ! -f "$src" ]]; then
        echo "[rootfs] WARNING: $src not found, skipping"
        continue
    fi

    # Ensure parent directory exists
    dest_dir="$(dirname "$dest")"
    ensure_dir "$dest_dir"

    # Queue the write
    debugfs_run "write $src $dest"

    # Auto-chmod executables in /bin and /lib
    if [[ "$dest" == /bin/* || "$dest" == /lib/* ]]; then
        CHMOD_CMDS+="set_inode_field $dest mode 0100755\n"
    fi
done < "$MANIFEST"

# Batch chmod
if [[ -n "$CHMOD_CMDS" ]]; then
    echo "[rootfs] Setting permissions..."
    printf "$CHMOD_CMDS" | $DEBUGFS -w "$ROOTFS_IMG" >/dev/null 2>&1
fi

# ── Optional assets ──────────────────────────────────────────────────────────
ensure_dir "/usr"
ensure_dir "/usr/share"

for raw_file in "$WALLPAPER_RAW" "$LOGO_RAW" "$CLAUDE_RAW"; do
    if [[ -n "$raw_file" && -f "$raw_file" && -s "$raw_file" ]]; then
        name="$(basename "$raw_file")"
        debugfs_run "write $raw_file /usr/share/$name"
    fi
done

# TTF fonts
if [[ -d assets ]]; then
    ensure_dir "/usr/share/fonts"
    for ttf in assets/*.ttf; do
        [[ -f "$ttf" ]] || continue
        name="$(basename "$ttf")"
        debugfs_run "write $ttf /usr/share/fonts/$name"
    done
fi

# ── Kernel binary for installed-system boot ──────────────────────────────────
ensure_dir "/boot"
ensure_dir "/boot/grub"
debugfs_run "write $KERNEL_ELF /boot/aegis.elf"

echo "[rootfs] Done: $ROOTFS_IMG"
