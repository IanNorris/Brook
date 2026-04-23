#!/usr/bin/env bash
# Create the persistent Brook OS disk image (FAT32, 256MB).
#
# This is a one-time setup script. The image lives at /workspace/brook_disk.img
# (outside the repo) and persists across builds. Use update_disk.sh to sync
# build artifacts into it.
#
# Usage:
#   scripts/create_disk.sh [size_mb]    # default: 256
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
DISK_IMG="${BROOK_DISK_IMG:-${ROOT_DIR}/brook_disk.img}"
SIZE_MB="${1:-256}"

if [ -f "${DISK_IMG}" ]; then
    echo "Disk image already exists at ${DISK_IMG}"
    echo "Delete it first if you want to recreate: rm ${DISK_IMG}"
    exit 1
fi

echo "Creating ${SIZE_MB}MB FAT32 disk image at ${DISK_IMG}..."

# Create empty image
dd if=/dev/zero of="${DISK_IMG}" bs=1M count="${SIZE_MB}" status=none

# Format as FAT32
mkfs.fat -F 32 -n "BROOKDISK" "${DISK_IMG}"

# Create directory structure
mmd -i "${DISK_IMG}" ::DRIVERS
mmd -i "${DISK_IMG}" ::BIN
mmd -i "${DISK_IMG}" ::MUSIC

# Write BROOK.MNT — tells the kernel to mount this volume at /boot
echo -n "/boot" | mcopy -i "${DISK_IMG}" - "::BROOK.MNT"

echo "Disk image created: ${DISK_IMG}"
echo ""
echo "Populate it with:"
echo "  scripts/update_disk.sh              # sync build artifacts"
echo "  mcopy -i ${DISK_IMG} <file> ::<DEST>  # add individual files"
echo "  mdir  -i ${DISK_IMG} ::               # list contents"
