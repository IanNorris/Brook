#!/usr/bin/env bash
# Create an ext2 disk image pre-populated with a Nix store closure.
#
# Usage:
#   scripts/create_nix_disk.sh [size_mb] [nix_attr...]
#   scripts/create_nix_disk.sh 128 pkgsMusl.hello pkgsMusl.coreutils
#
# Defaults: 512MB disk, pkgsMusl.hello + pkgsMusl.coreutils
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

# Parse size (first numeric arg) and packages
SIZE_MB=128
PACKAGES=()
for arg in "$@"; do
    if [[ "$arg" =~ ^[0-9]+$ ]]; then
        SIZE_MB="$arg"
    else
        PACKAGES+=("$arg")
    fi
done
if [ ${#PACKAGES[@]} -eq 0 ]; then
    PACKAGES=(pkgsMusl.hello pkgsMusl.coreutils)
fi

DISK_IMG="${BROOK_NIX_DISK:-${ROOT_DIR}/brook_nix_disk.img}"

echo "Creating ${SIZE_MB}MB Nix store disk at ${DISK_IMG}..."

# Create and format
dd if=/dev/zero of="${DISK_IMG}" bs=1M count="${SIZE_MB}" status=progress 2>&1
mkfs.ext2 -q -b 4096 -L "NIXSTORE" "${DISK_IMG}"

# Build all requested packages and collect closures
echo "Building packages: ${PACKAGES[*]}..."
ALL_PATHS=()
for pkg in "${PACKAGES[@]}"; do
    PKG_PATH=$(nix-build '<nixpkgs>' -A "$pkg" --no-out-link 2>/dev/null)
    echo "  ${pkg}: ${PKG_PATH}"
    ALL_PATHS+=("$PKG_PATH")
done

# Get full transitive closure (deduplicated)
CLOSURE=$(nix-store -qR "${ALL_PATHS[@]}" | sort -u)
echo "  Closure ($(echo "$CLOSURE" | wc -l) paths):"
for p in ${CLOSURE}; do
    SIZE=$(du -sh "$p" | cut -f1)
    echo "    ${p}  (${SIZE})"
done

# Mount the disk image and populate it
MOUNT_DIR=$(mktemp -d)
echo "Mounting at ${MOUNT_DIR}..."

# Use fuse2fs if available (no root needed), otherwise debugfs
if command -v fuse2fs &>/dev/null; then
    fuse2fs -o rw "${DISK_IMG}" "${MOUNT_DIR}"
    FUSE=1
else
    echo "Warning: fuse2fs not available, using debugfs (slower)"
    FUSE=0
fi

if [ "${FUSE}" -eq 1 ]; then
    # Create BROOK.MNT
    echo -n "/nix" > "${MOUNT_DIR}/BROOK.MNT"

    # Create store directory
    mkdir -p "${MOUNT_DIR}/store"

    # Copy each store path
    for p in ${CLOSURE}; do
        BASENAME=$(basename "$p")
        echo "  Copying ${BASENAME}..."
        cp -a "$p" "${MOUNT_DIR}/store/${BASENAME}"
    done

    # Sync and unmount
    sync
    fusermount -u "${MOUNT_DIR}"
else
    # debugfs approach: create directories and write files recursively
    # First create BROOK.MNT
    TMPFILE=$(mktemp)
    echo -n "/nix" > "${TMPFILE}"
    debugfs -w "${DISK_IMG}" -R "write ${TMPFILE} BROOK.MNT" 2>/dev/null
    rm -f "${TMPFILE}"

    # Create store directory
    debugfs -w "${DISK_IMG}" -R "mkdir store" 2>/dev/null

    # For each store path, we need to recursively create dirs and write files
    for p in ${CLOSURE}; do
        BASENAME=$(basename "$p")
        echo "  Copying ${BASENAME}..."

        # Create the store hash dir
        debugfs -w "${DISK_IMG}" -R "mkdir store/${BASENAME}" 2>/dev/null

        # Walk the directory tree
        (cd "$p" && find . -type d) | while read -r dir; do
            if [ "$dir" != "." ]; then
                RELDIR="${dir#./}"
                debugfs -w "${DISK_IMG}" -R "mkdir store/${BASENAME}/${RELDIR}" 2>/dev/null
            fi
        done

        (cd "$p" && find . -type f) | while read -r file; do
            RELFILE="${file#./}"
            debugfs -w "${DISK_IMG}" -R "write ${p}/${RELFILE} store/${BASENAME}/${RELFILE}" 2>/dev/null
        done

        # Handle symlinks
        (cd "$p" && find . -type l) | while read -r link; do
            RELLINK="${link#./}"
            TARGET=$(readlink "$p/${RELLINK}")
            debugfs -w "${DISK_IMG}" -R "symlink store/${BASENAME}/${RELLINK} ${TARGET}" 2>/dev/null
        done
    done
fi

rmdir "${MOUNT_DIR}" 2>/dev/null || true

echo ""
echo "Nix store disk created: ${DISK_IMG}"
echo "  Mount point: /nix (via BROOK.MNT)"
echo "  Packages: ${PACKAGES[*]}"
for pkg_path in "${ALL_PATHS[@]}"; do
    echo "  Test: run ${pkg_path}/bin/..."
done
