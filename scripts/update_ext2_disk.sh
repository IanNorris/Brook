#!/usr/bin/env bash
# Sync build artifacts into the ext2 Brook OS disk image.
#
# Usage:
#   scripts/update_ext2_disk.sh [--create]
#
# Uses fuse2fs (from e2fsprogs) to mount the image without sudo.
# Falls back to debugfs if fuse2fs is unavailable.
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

echo "Syncing to ext2 disk: ${DISK_IMG}"

# --- Mount the image with fuse2fs (no sudo required) ---
MNTDIR=$(mktemp -d)
cleanup() { sync; fusermount -u "${MNTDIR}" 2>/dev/null || fusermount -uz "${MNTDIR}" 2>/dev/null || true; rmdir "${MNTDIR}" 2>/dev/null || true; }
trap cleanup EXIT

if ! command -v fuse2fs >/dev/null 2>&1; then
    echo "ERROR: fuse2fs not found. Add e2fsprogs-fuse2fs to shell.nix."
    exit 1
fi

fuse2fs -o rw,fakeroot "${DISK_IMG}" "${MNTDIR}"
echo "  mounted at ${MNTDIR}"

# Helper: copy a file, creating parent dirs as needed
write_file() {
    local src="$1"
    local dest="$2"
    if [ -f "$src" ]; then
        mkdir -p "${MNTDIR}/$(dirname "${dest}")"
        cp "$src" "${MNTDIR}/${dest}"
        echo "  → ${dest}"
    fi
}

# Ensure directories exist
mkdir -p "${MNTDIR}/drivers"
mkdir -p "${MNTDIR}/bin"
mkdir -p "${MNTDIR}/lib"
mkdir -p "${MNTDIR}/etc"
mkdir -p "${MNTDIR}/tmp"
mkdir -p "${MNTDIR}/dev"
mkdir -p "${MNTDIR}/usr/lib"

# BROOK.CFG
BROOK_CFG="${BUILD_DIR}/esp/BROOK.CFG"
[ -f "${BROOK_CFG}" ] && write_file "${BROOK_CFG}" "BROOK.CFG"

# BROOK.MNT
TMPFILE=$(mktemp)
echo -n "/data" > "${TMPFILE}"
write_file "${TMPFILE}" "BROOK.MNT"
rm -f "${TMPFILE}"

# Phase 2 driver modules
if [ -d "${BUILD_DIR}/kernel/modules" ]; then
    for mod in "${BUILD_DIR}/kernel/modules"/*.mod; do
        [ -f "$mod" ] || continue
        write_file "$mod" "drivers/$(basename "$mod")"
    done
fi

# Userspace binaries
[ -f "${ROOT_DIR}/busybox_static" ] && write_file "${ROOT_DIR}/busybox_static" "bin/busybox"
[ -f "${ROOT_DIR}/bash_dynamic" ]   && write_file "${ROOT_DIR}/bash_dynamic" "bin/bash"

# Dynamic libraries
if [ -d "${ROOT_DIR}/dynlibs" ]; then
    for lib in "${ROOT_DIR}/dynlibs"/*.so*; do
        [ -f "$lib" ] || continue
        write_file "$lib" "lib/$(basename "$lib")"
    done
fi

# DOOM
[ -f "${ROOT_DIR}/doom1.wad" ] && write_file "${ROOT_DIR}/doom1.wad" "doom1.wad"
[ -f "${ROOT_DIR}/doomgeneric_enkel/doomgeneric" ] && write_file "${ROOT_DIR}/doomgeneric_enkel/doomgeneric" "bin/doom"

# Wallpaper
[ -f "${ROOT_DIR}/wallpaper/wallpaper.raw" ] && write_file "${ROOT_DIR}/wallpaper/wallpaper.raw" "wallpaper.raw"

# Quake 2
Q2_BIN="${ROOT_DIR}/build/quake2/quake2"
Q2_PAK="${BROOK_Q2_PAK:-${ROOT_DIR}/../q2demo/Install/Data/baseq2/pak0.pak}"
Q2_MUSIC="${BROOK_Q2_MUSIC:-${ROOT_DIR}/../q2ost}"
Q2_PLAYERS="${BROOK_Q2_PLAYERS:-${ROOT_DIR}/../q2demo/Install/Data/baseq2/players}"
if [ -f "${Q2_BIN}" ]; then
    mkdir -p "${MNTDIR}/games/quake2/baseq2/music" 2>/dev/null || true
    write_file "${Q2_BIN}" "games/quake2/quake2"
    if [ -f "${Q2_PAK}" ]; then
        write_file "${Q2_PAK}" "games/quake2/baseq2/pak0.pak"
    else
        echo "  (Q2 pak not found at ${Q2_PAK} — set BROOK_Q2_PAK to override)"
    fi
    # OGG music tracks
    if [ -d "${Q2_MUSIC}" ]; then
        for ogg in "${Q2_MUSIC}"/*.ogg; do
            [ -f "$ogg" ] || continue
            write_file "$ogg" "games/quake2/baseq2/music/$(basename "$ogg")"
        done
    fi
    # Player models (live outside pak0.pak in the Q2 demo distribution)
    if [ -d "${Q2_PLAYERS}" ]; then
        echo "  Q2 player models from ${Q2_PLAYERS}"
        mkdir -p "${MNTDIR}/games/quake2/baseq2/players"
        cp -r "${Q2_PLAYERS}/." "${MNTDIR}/games/quake2/baseq2/players/"
    else
        echo "  (Q2 players dir not found at ${Q2_PLAYERS} — set BROOK_Q2_PLAYERS to override)"
    fi
fi

# TCC
[ -f "${ROOT_DIR}/tcc_build/tcc" ] && write_file "${ROOT_DIR}/tcc_build/tcc" "bin/tcc"

# Test apps
for app in ansi_test snake game2048; do
    [ -f "${ROOT_DIR}/tcc_build/${app}" ] && write_file "${ROOT_DIR}/tcc_build/${app}" "bin/${app}"
done

# Symlinks
if [ ! -L "${MNTDIR}/sh" ]; then
    ln -s bin/busybox "${MNTDIR}/sh" 2>/dev/null && echo "  → sh -> bin/busybox (symlink)" || true
fi

echo "Unmounting..."
# Explicit sync before unmount to ensure all writes are flushed
sync
fusermount -u "${MNTDIR}" || { echo "WARNING: fusermount failed, retrying with -z"; fusermount -uz "${MNTDIR}"; }
rmdir "${MNTDIR}" 2>/dev/null || true
trap - EXIT

echo "Done."
