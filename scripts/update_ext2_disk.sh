#!/usr/bin/env bash
# Sync build artifacts into the ext2 Brook OS disk image.
#
# Usage:
#   scripts/update_ext2_disk.sh [--create]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
DISK_IMG="${BROOK_EXT2_DISK:-${ROOT_DIR}/brook_ext2_disk.img}"
BUILD_DIR="${ROOT_DIR}/build/release"

AUTO_CREATE=0
for arg in "$@"; do
    [ "$arg" = "--create" ] && AUTO_CREATE=1
done

if [ ! -f "${DISK_IMG}" ]; then
    if [ "${AUTO_CREATE}" -eq 1 ]; then
        "${SCRIPT_DIR}/create_ext2_disk.sh"
    else
        echo "Ext2 disk image not found at ${DISK_IMG}"
        echo "Run: scripts/create_ext2_disk.sh"
        exit 1
    fi
fi

# Auto-fetch userspace binaries if missing
if [ ! -f "${ROOT_DIR}/busybox_static" ] || [ ! -f "${ROOT_DIR}/bash_dynamic" ]; then
    echo "Userspace binaries missing, running fetch_userspace.sh..."
    "${SCRIPT_DIR}/fetch_userspace.sh" || echo "  (fetch_userspace.sh failed, continuing with what's available)"
fi

# Helper: write a file into the ext2 image, creating parent dirs as needed
write_file() {
    local src="$1"
    local dest="$2"
    if [ -f "$src" ]; then
        debugfs -w "${DISK_IMG}" -R "write ${src} ${dest}" 2>/dev/null
        echo "  → ${dest}"
    fi
}

echo "Syncing to ext2 disk: ${DISK_IMG}"

# Ensure directories exist
debugfs -w "${DISK_IMG}" <<'EOF' 2>/dev/null
mkdir drivers
mkdir bin
mkdir lib
mkdir etc
mkdir tmp
mkdir dev
mkdir usr
mkdir usr/lib
EOF

# BROOK.CFG
BROOK_CFG="${BUILD_DIR}/esp/BROOK.CFG"
if [ -f "${BROOK_CFG}" ]; then
    write_file "${BROOK_CFG}" "BROOK.CFG"
fi

# BROOK.MNT
TMPDIR=$(mktemp -d)
echo -n "/boot" > "${TMPDIR}/BROOK.MNT"
write_file "${TMPDIR}/BROOK.MNT" "BROOK.MNT"
rm -rf "${TMPDIR}"

# Phase 2 driver modules
if [ -d "${BUILD_DIR}/kernel/modules" ]; then
    for mod in "${BUILD_DIR}/kernel/modules"/*.mod; do
        [ -f "$mod" ] || continue
        name=$(basename "$mod")
        write_file "$mod" "drivers/${name}"
    done
fi

# Userspace binaries
[ -f "${ROOT_DIR}/busybox_static" ] && write_file "${ROOT_DIR}/busybox_static" "bin/busybox"
[ -f "${ROOT_DIR}/bash_dynamic" ]   && write_file "${ROOT_DIR}/bash_dynamic" "bin/bash"

# Dynamic libraries
if [ -d "${ROOT_DIR}/dynlibs" ]; then
    for lib in "${ROOT_DIR}/dynlibs"/*.so*; do
        [ -f "$lib" ] || continue
        name=$(basename "$lib")
        write_file "$lib" "lib/${name}"
    done
fi

# DOOM
[ -f "${ROOT_DIR}/doom1.wad" ]     && write_file "${ROOT_DIR}/doom1.wad" "doom1.wad"
[ -f "${ROOT_DIR}/doomgeneric_enkel/doomgeneric" ] && write_file "${ROOT_DIR}/doomgeneric_enkel/doomgeneric" "bin/doom"

# Wallpaper
[ -f "${ROOT_DIR}/wallpaper/wallpaper.raw" ] && write_file "${ROOT_DIR}/wallpaper/wallpaper.raw" "wallpaper.raw"

# TCC
[ -f "${ROOT_DIR}/tcc_build/tcc" ] && write_file "${ROOT_DIR}/tcc_build/tcc" "bin/tcc"

# Test apps
for app in ansi_test snake game2048; do
    [ -f "${ROOT_DIR}/tcc_build/${app}" ] && write_file "${ROOT_DIR}/tcc_build/${app}" "bin/${app}"
done

# Create symlinks that Nix needs (test symlink support!)
debugfs -w "${DISK_IMG}" -R "symlink sh bin/busybox" 2>/dev/null && echo "  → sh -> bin/busybox (symlink)" || true

echo "Done."
