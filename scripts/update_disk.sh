#!/usr/bin/env bash
# Sync build artifacts and user content into the persistent Brook disk image.
#
# This uses mcopy's overwrite mode to update files that have changed,
# similar to a one-way rsync. It's safe to run repeatedly.
#
# Usage:
#   scripts/update_disk.sh [--create]   # --create will auto-create if missing
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
DISK_IMG="${BROOK_DISK_IMG:-${ROOT_DIR}/brook_disk.img}"

AUTO_CREATE=0
for arg in "$@"; do
    [ "$arg" = "--create" ] && AUTO_CREATE=1
done

if [ ! -f "${DISK_IMG}" ]; then
    if [ "${AUTO_CREATE}" -eq 1 ]; then
        "${SCRIPT_DIR}/create_disk.sh"
    else
        echo "Disk image not found at ${DISK_IMG}"
        echo "Run: scripts/create_disk.sh"
        exit 1
    fi
fi

# mcopy -o = overwrite existing files without prompting
# mcopy -n = no overwrite (skip existing) — we use -o for sync semantics

sync_file() {
    local src="$1"
    local dest="$2"
    if [ -f "$src" ]; then
        mcopy -o -i "${DISK_IMG}" "$src" "::${dest}"
        echo "  synced: ${dest} ($(stat -c%s "$src") bytes)"
    fi
}

sync_dir_contents() {
    local src_dir="$1"
    local dest_dir="$2"
    local pattern="${3:-*}"

    if [ ! -d "$src_dir" ]; then
        return
    fi

    # Ensure destination directory exists (mmd returns non-zero if it already exists)
    mmd -D s -i "${DISK_IMG}" "::${dest_dir}" 2>/dev/null || true

    for f in "${src_dir}"/${pattern}; do
        [ -f "$f" ] || continue
        local name
        name="$(basename "$f" | tr '[:lower:]' '[:upper:]')"
        mcopy -o -i "${DISK_IMG}" "$f" "::${dest_dir}/${name}"
        echo "  synced: ${dest_dir}/${name} ($(stat -c%s "$f") bytes)"
    done
}

echo "Updating disk image: ${DISK_IMG}"

# --- Driver modules (from most recent build) ---
# Check release first, then debug
for build_type in release debug; do
    mod_dir="${ROOT_DIR}/build/${build_type}/kernel/drivers"
    if [ -d "$mod_dir" ] && ls "$mod_dir"/*.mod &>/dev/null; then
        echo "Drivers (from ${build_type} build):"
        sync_dir_contents "$mod_dir" "DRIVERS" "*.mod"
        break
    fi
done

# --- User binaries ---
echo "User binaries:"
# Check build/*/user/ directories
for build_type in release debug; do
    user_dir="${ROOT_DIR}/build/${build_type}/user"
    if [ -d "$user_dir" ]; then
        mmd -D s -i "${DISK_IMG}" "::BIN" 2>/dev/null || true
        sync_dir_contents "$user_dir" "BIN"
        break
    fi
done

# --- Test apps (static musl binaries) ---
apps_dir="${ROOT_DIR}/build/apps"
if [ -d "$apps_dir" ] && ls "$apps_dir"/* &>/dev/null 2>&1; then
    echo "Apps:"
    mmd -D s -i "${DISK_IMG}" "::BIN" 2>/dev/null || true
    sync_dir_contents "$apps_dir" "BIN"
fi

# --- DOOM binary ---
doom_bin="${ROOT_DIR}/build/doom/doom"
if [ -f "$doom_bin" ]; then
    echo "DOOM:"
    sync_file "$doom_bin" "DOOM"
fi

# --- DOOM WAD (check workspace root) ---
for wad_path in "${ROOT_DIR}/doom1.wad" "/workspace/doom1.wad"; do
    if [ -f "$wad_path" ]; then
        echo "DOOM WAD:"
        sync_file "$wad_path" "DOOM1.WAD"
        break
    fi
done

# --- Static binaries from nix (busybox etc.) ---
# These are added manually once; this section just reports what's present.

# --- Dynamic libraries (/lib) ---
dynlibs_dir="${ROOT_DIR}/dynlibs"
if [ -d "$dynlibs_dir" ] && ls "$dynlibs_dir"/*.so* &>/dev/null 2>&1; then
    echo "Dynamic libraries:"
    mmd -D s -i "${DISK_IMG}" "::LIB" 2>/dev/null || true
    for f in "$dynlibs_dir"/*.so*; do
        [ -f "$f" ] || continue
        local_name="$(basename "$f")"
        mcopy -o -i "${DISK_IMG}" "$f" "::LIB/${local_name}"
        echo "  synced: LIB/${local_name} ($(stat -c%s "$f") bytes)"
    done
fi

# --- Dynamic bash binary ---
bash_dyn="${ROOT_DIR}/bash_dynamic"
if [ -f "$bash_dyn" ]; then
    echo "Bash (dynamic):"
    mmd -D s -i "${DISK_IMG}" "::BIN" 2>/dev/null || true
    sync_file "$bash_dyn" "BIN/BASH"
fi

# --- Boot script (INIT.RC) ---
init_rc="${ROOT_DIR}/data/INIT.RC"
if [ -f "$init_rc" ]; then
    echo "Boot script:"
    sync_file "$init_rc" "INIT.RC"
fi

# --- Data files (Lua scripts, etc.) ---
data_scripts="${ROOT_DIR}/data/scripts"
if [ -d "$data_scripts" ]; then
    for f in "$data_scripts"/*.lua; do
        [ -f "$f" ] || continue
        dname="$(basename "$f" | tr '[:lower:]' '[:upper:]')"
        mcopy -o -i "${DISK_IMG}" "$f" "::${dname}"
        echo "  synced data: ${dname}"
    done
fi

echo ""
echo "Current disk contents:"
mdir -i "${DISK_IMG}" :: 2>&1 | grep -v "^$"
