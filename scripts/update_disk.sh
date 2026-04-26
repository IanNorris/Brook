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

# Auto-fetch userspace binaries if missing
if [ ! -f "${ROOT_DIR}/busybox_static" ] || [ ! -f "${ROOT_DIR}/bash_dynamic" ]; then
    echo "Userspace binaries missing, running fetch_userspace.sh..."
    "${SCRIPT_DIR}/fetch_userspace.sh" || echo "  (fetch_userspace.sh failed, continuing with what's available)"
fi

# mcopy -o = overwrite existing files without prompting
# mcopy -n = no overwrite (skip existing) — we use -o for sync semantics

sync_file() {
    local src="$1"
    local dest="$2"
    if [ -f "$src" ]; then
        mcopy_safe -o -i "${DISK_IMG}" "$src" "::${dest}"
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
        mcopy_safe -o -i "${DISK_IMG}" "$f" "::${dest_dir}/${name}"
        echo "  synced: ${dest_dir}/${name} ($(stat -c%s "$f") bytes)"
    done
}

echo "Updating disk image: ${DISK_IMG}"

# --- Concurrency guard: serialize update_disk runs against this image ---
# Without this, two concurrent invocations (or one that was killed mid-run
# leaving stale fcntl state) can corrupt the FAT, after which subsequent
# mcopy invocations spin forever traversing a broken cluster chain.
LOCKFILE="${DISK_IMG}.lock"
exec 9>"${LOCKFILE}"
if ! flock -n 9; then
    echo "Another update_disk.sh is running for ${DISK_IMG} (lock held)."
    echo "Wait for it to finish, or remove ${LOCKFILE} if stale."
    exit 1
fi

# --- Filesystem health precheck ---
# Catch corruption early, before we kick off ~50 mcopy invocations that
# would otherwise loop forever on a broken FAT.  fsck.fat -a auto-repairs
# trivial issues (orphan clusters, dirty bit) which mcopy's own writes
# leave behind when it's interrupted; only hard corruption fails the run.
if command -v fsck.fat >/dev/null 2>&1; then
    # NB: fsck.fat returns 1 on "errors corrected" — that's a success for us
    # but `set -e` would kill the script before we could inspect $?.
    fsck_rc=0
    fsck.fat -a "${DISK_IMG}" >/tmp/brook_fsck.$$ 2>&1 || fsck_rc=$?
    # fsck.fat exit codes: 0=clean, 1=errors corrected, 2=errors not corrected
    if [ $fsck_rc -ge 2 ]; then
        echo "FAT filesystem corrupted in ${DISK_IMG} (unfixable):"
        sed 's/^/  /' /tmp/brook_fsck.$$
        rm -f /tmp/brook_fsck.$$
        echo ""
        echo "To recover:"
        echo "  rm ${DISK_IMG}"
        echo "  scripts/create_disk.sh"
        echo "  scripts/update_disk.sh"
        exit 1
    fi
    if [ $fsck_rc -eq 1 ]; then
        echo "  fsck.fat: auto-repaired residual issues from a prior killed run"
    fi
    rm -f /tmp/brook_fsck.$$
fi

# --- Per-mcopy timeout wrapper ---
# A pathological FAT can make a single mcopy spin forever consuming
# unbounded memory (we've observed runaway parents with 30+ stuck mcopy
# children).  Hard-cap each invocation; if any single file takes > 30s
# something is wrong.
mcopy_safe() {
    local rc=0
    timeout --signal=KILL 30 mcopy "$@" || rc=$?
    if [ $rc -eq 137 ] || [ $rc -eq 124 ]; then
        echo "ERROR: mcopy timed out (>30s) — disk image likely corrupt." >&2
        echo "  Args: $*" >&2
        echo "  Recover with: rm ${DISK_IMG} && scripts/create_disk.sh && scripts/update_disk.sh" >&2
        exit 1
    fi
    return $rc
}

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

# --- Audio tools (wavplay, tone) ---
audio_dir="${ROOT_DIR}/tools/wavplay"
for tool in wavplay tone; do
    if [ -f "$audio_dir/$tool" ]; then
        echo "Audio tools:"
        mmd -D s -i "${DISK_IMG}" "::BIN" 2>/dev/null || true
        TOOL_UPPER=$(echo "$tool" | tr '[:lower:]' '[:upper:]')
        sync_file "$audio_dir/$tool" "BIN/$TOOL_UPPER"
    fi
done

# --- MP3 player ---
mp3play_bin="${ROOT_DIR}/tools/mp3play/mp3play"
if [ -f "$mp3play_bin" ]; then
    echo "MP3 player:"
    mmd -D s -i "${DISK_IMG}" "::BIN" 2>/dev/null || true
    sync_file "$mp3play_bin" "BIN/MP3PLAY"
fi

# --- sinetest tool ---
sinetest_bin="${ROOT_DIR}/tools/sinetest/sinetest"
if [ -f "$sinetest_bin" ]; then
    echo "Sine test:"
    mmd -D s -i "${DISK_IMG}" "::BIN" 2>/dev/null || true
    sync_file "$sinetest_bin" "BIN/SINETEST"
fi

# --- nix-install tool ---
nix_install_bin="${ROOT_DIR}/tools/nix-install/nix-install"
if [ -f "$nix_install_bin" ]; then
    echo "nix-install:"
    mmd -D s -i "${DISK_IMG}" "::BIN" 2>/dev/null || true
    sync_file "$nix_install_bin" "BIN/NIX-INSTALL"
fi

# --- profile tool (profiler control wrapper) ---
profile_bin="${ROOT_DIR}/tools/profile/profile"
if [ -f "$profile_bin" ]; then
    echo "profile:"
    mmd -D s -i "${DISK_IMG}" "::BIN" 2>/dev/null || true
    sync_file "$profile_bin" "BIN/PROFILE"
fi

# --- Static binaries from nix (busybox etc.) ---
busybox_static="${ROOT_DIR}/busybox_static"
if [ -f "$busybox_static" ]; then
    echo "Busybox (static):"
    mmd -D s -i "${DISK_IMG}" "::BIN" 2>/dev/null || true
    # Delete any existing uppercase BUSYBOX to avoid conflicts
    mdel -i "${DISK_IMG}" "::BIN/BUSYBOX" 2>/dev/null || true
    mcopy_safe -o -i "${DISK_IMG}" "$busybox_static" "::BIN/busybox"
    echo "  synced: BIN/busybox ($(stat -c%s "$busybox_static") bytes)"
fi

# --- Dynamic libraries (/lib) ---
dynlibs_dir="${ROOT_DIR}/dynlibs"
if [ -d "$dynlibs_dir" ] && ls "$dynlibs_dir"/*.so* &>/dev/null 2>&1; then
    echo "Dynamic libraries:"
    mmd -D s -i "${DISK_IMG}" "::LIB" 2>/dev/null || true
    for f in "$dynlibs_dir"/*.so*; do
        [ -f "$f" ] || continue
        local_name="$(basename "$f")"
        mcopy_safe -o -i "${DISK_IMG}" "$f" "::LIB/${local_name}"
        echo "  synced: LIB/${local_name} ($(stat -c%s "$f") bytes)"
        # musl unifies libc and the dynamic linker — ld-musl-x86_64.so.1
        # is normally a symlink to libc.so.  FAT has no symlinks, so
        # publish a second copy under the canonical PT_INTERP name so
        # dynamic ELFs (bash etc.) can find their interpreter.
        if [ "$local_name" = "libc.so" ]; then
            mcopy_safe -o -i "${DISK_IMG}" "$f" "::LIB/ld-musl-x86_64.so.1"
            echo "  synced: LIB/ld-musl-x86_64.so.1 (alias of libc.so)"
        fi
    done
fi

# --- Dynamic bash binary ---
bash_dyn="${ROOT_DIR}/bash_dynamic"
if [ -f "$bash_dyn" ]; then
    echo "Bash (dynamic):"
    mmd -D s -i "${DISK_IMG}" "::BIN" 2>/dev/null || true
    sync_file "$bash_dyn" "BIN/BASH"
fi

# --- TCC compiler and sysroot ---
tcc_sysroot="${ROOT_DIR}/tcc_sysroot"
if [ -d "$tcc_sysroot" ] && [ -f "$tcc_sysroot/tcc" ]; then
    echo "TCC compiler:"
    mmd -D s -i "${DISK_IMG}" "::BIN" 2>/dev/null || true
    mcopy_safe -o -i "${DISK_IMG}" "$tcc_sysroot/tcc" "::BIN/tcc"
    echo "  synced: BIN/tcc ($(stat -c%s "$tcc_sysroot/tcc") bytes)"

    # TCC library dir (libtcc1.a + tcc includes)
    mmd -D s -i "${DISK_IMG}" "::TCC" 2>/dev/null || true
    mmd -D s -i "${DISK_IMG}" "::TCC/lib" 2>/dev/null || true
    mmd -D s -i "${DISK_IMG}" "::TCC/lib/tcc" 2>/dev/null || true
    mmd -D s -i "${DISK_IMG}" "::TCC/lib/tcc/include" 2>/dev/null || true
    mcopy_safe -o -i "${DISK_IMG}" "$tcc_sysroot/lib/tcc/libtcc1.a" "::TCC/lib/tcc/libtcc1.a"
    echo "  synced: TCC/lib/tcc/libtcc1.a"
    for f in "$tcc_sysroot"/lib/tcc/include/*.h; do
        [ -f "$f" ] || continue
        mcopy_safe -o -i "${DISK_IMG}" "$f" "::TCC/lib/tcc/include/$(basename "$f")"
    done
    echo "  synced: TCC/lib/tcc/include/*.h"

    # Musl include tree
    mmd -D s -i "${DISK_IMG}" "::TCC/include" 2>/dev/null || true
    # Top-level headers
    for f in "$tcc_sysroot"/include/*.h; do
        [ -f "$f" ] || continue
        mcopy_safe -o -i "${DISK_IMG}" "$f" "::TCC/include/$(basename "$f")"
    done
    # Subdirectories (sys/, bits/, arpa/, net/, netinet/, etc.)
    for subdir in "$tcc_sysroot"/include/*/; do
        [ -d "$subdir" ] || continue
        dname="$(basename "$subdir")"
        mmd -D s -i "${DISK_IMG}" "::TCC/include/${dname}" 2>/dev/null || true
        for f in "$subdir"*; do
            [ -f "$f" ] || continue
            mcopy_safe -o -i "${DISK_IMG}" "$f" "::TCC/include/${dname}/$(basename "$f")"
        done
    done
    echo "  synced: TCC/include/ (musl headers)"

    # Musl CRT and libc.a
    for f in crt1.o crti.o crtn.o libc.a; do
        if [ -f "$tcc_sysroot/lib/$f" ]; then
            mcopy_safe -o -i "${DISK_IMG}" "$tcc_sysroot/lib/$f" "::TCC/lib/$f"
            echo "  synced: TCC/lib/$f"
        fi
    done
fi

# --- TCC test source files ---
tcc_test_dir="${ROOT_DIR}/data/tcc_test"
if [ -d "$tcc_test_dir" ]; then
    mmd -D s -i "${DISK_IMG}" "::SRC" 2>/dev/null || true
    for f in "$tcc_test_dir"/*.c "$tcc_test_dir"/*.sh "$tcc_test_dir"/cc; do
        [ -f "$f" ] || continue
        mcopy_safe -o -i "${DISK_IMG}" "$f" "::SRC/$(basename "$f")"
        echo "  synced: SRC/$(basename "$f")"
    done
    # Also put cc wrapper in BIN/ for easy access
    if [ -f "$tcc_test_dir/cc" ]; then
        mcopy_safe -o -i "${DISK_IMG}" "$tcc_test_dir/cc" "::BIN/cc"
        echo "  synced: BIN/cc"
    fi
fi

# --- Boot script (INIT.RC) ---
init_rc="${ROOT_DIR}/data/INIT.RC"
if [ -f "$init_rc" ]; then
    echo "Boot script:"
    sync_file "$init_rc" "INIT.RC"
fi

# --- Data files (Lua scripts, boot configs) ---
data_scripts="${ROOT_DIR}/data/scripts"
if [ -d "$data_scripts" ]; then
    mmd -D s -i "${DISK_IMG}" "::SCRIPTS" 2>/dev/null || true
    for f in "$data_scripts"/*.lua "$data_scripts"/*.rc; do
        [ -f "$f" ] || continue
        dname="$(basename "$f" | tr '[:lower:]' '[:upper:]')"
        # Top-level copy preserved for legacy callers (INIT.RC, etc).
        mcopy_safe -o -i "${DISK_IMG}" "$f" "::${dname}"
        # Also place under /SCRIPTS/ so shortcuts can `source /boot/SCRIPTS/<name>`.
        mcopy_safe -o -i "${DISK_IMG}" "$f" "::SCRIPTS/${dname}"
        echo "  synced data: ${dname} (root + SCRIPTS/)"
    done
fi

# --- App shortcuts ---
shortcuts_dir="${ROOT_DIR}/data/shortcuts"
if [ -d "$shortcuts_dir" ]; then
    mmd -D s -i "${DISK_IMG}" "::SHORTCUTS" 2>/dev/null || true
    for f in "$shortcuts_dir"/*.rc; do
        [ -f "$f" ] || continue
        dname="$(basename "$f" | tr '[:lower:]' '[:upper:]')"
        mcopy_safe -o -i "${DISK_IMG}" "$f" "::SHORTCUTS/${dname}"
        echo "  synced shortcut: SHORTCUTS/${dname}"
    done
fi

# --- Wallpaper ---
wallpaper="${ROOT_DIR}/data/wallpaper.raw"
if [ -f "$wallpaper" ]; then
    echo "Wallpaper:"
    sync_file "$wallpaper" "WALLPAPER.RAW"
fi

# --- User content (MP3s, etc.) ---
user_content_dir="${ROOT_DIR}/data/user_content"
if [ -d "$user_content_dir" ] && ls "$user_content_dir"/* &>/dev/null 2>&1; then
    echo "User content:"
    mmd -D s -i "${DISK_IMG}" "::MUSIC" 2>/dev/null || true
    for f in "$user_content_dir"/*; do
        [ -f "$f" ] || continue
        local_name="$(basename "$f")"
        # FAT filenames: truncate to 8.3 or use VFAT long names (mcopy handles this)
        mcopy_safe -o -i "${DISK_IMG}" "$f" "::MUSIC/${local_name}"
        echo "  synced: MUSIC/${local_name} ($(stat -c%s "$f") bytes)"
    done
fi

echo ""
echo "Current disk contents:"
mdir -i "${DISK_IMG}" :: 2>&1 | grep -v "^$"
