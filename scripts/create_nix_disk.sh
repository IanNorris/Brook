#!/usr/bin/env bash
# Create an ext2 disk image pre-populated with a Nix store closure.
#
# Usage:
#   scripts/create_nix_disk.sh [size_mb]    # default: 512
#
# The disk contains a BROOK.MNT file (mount point = /nix) and a /store
# directory with the full closure of pkgsMusl.hello (and its dependencies).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
SIZE_MB="${1:-512}"
DISK_IMG="${BROOK_NIX_DISK:-${ROOT_DIR}/brook_nix_disk.img}"

echo "Creating ${SIZE_MB}MB Nix store disk at ${DISK_IMG}..."

# Create and format
dd if=/dev/zero of="${DISK_IMG}" bs=1M count="${SIZE_MB}" status=progress 2>&1
mkfs.ext2 -q -b 4096 -L "NIXSTORE" "${DISK_IMG}"

# Build the musl hello closure
echo "Building pkgsMusl.hello closure..."
HELLO_PATH=$(nix-build '<nixpkgs>' -A pkgsMusl.hello --no-out-link 2>/dev/null)
echo "  hello: ${HELLO_PATH}"

# Get the full closure (all dependencies)
CLOSURE=$(nix-store -qR "${HELLO_PATH}")
echo "  Closure:"
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
echo "  Test: run /nix/store/$(basename ${HELLO_PATH})/bin/hello"
