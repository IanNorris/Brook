#!/usr/bin/env bash
# Create an ext4 Brook OS disk image.
#
# Usage:
#   scripts/create_ext4_disk.sh [size_mb]    # default: 2048
#
# Creates a raw ext4 image with journal, extents, and a BROOK.MNT marker
# file.  The kernel's ProbeAndMountDevice will detect this as ext4 and
# mount at the path specified in BROOK.MNT.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
DISK_IMG="${BROOK_EXT4_DISK:-${ROOT_DIR}/brook_ext4_disk.img}"
SIZE_MB="${1:-2048}"
MOUNT_PATH="${2:-/data}"

if [ -f "${DISK_IMG}" ]; then
    echo "Disk image already exists at ${DISK_IMG}"
    echo "Delete it first if you want to recreate: rm ${DISK_IMG}"
    exit 1
fi

echo "Creating ${SIZE_MB}MB ext4 disk image at ${DISK_IMG}..."

# Create empty image
dd if=/dev/zero of="${DISK_IMG}" bs=1M count="${SIZE_MB}" status=none

# Format as ext4 (with journal, extents, dir_index)
mkfs.ext4 -q -b 4096 -L "BROOKDISK" -O extent,dir_index "${DISK_IMG}"

# Create directory structure and BROOK.MNT using debugfs
# (debugfs works with ext4 images)
debugfs -w "${DISK_IMG}" <<'EOF'
mkdir drivers
mkdir bin
EOF

# Write BROOK.MNT (tells kernel which path to mount at)
TMPDIR=$(mktemp -d)
echo -n "${MOUNT_PATH}" > "${TMPDIR}/BROOK.MNT"
debugfs -w "${DISK_IMG}" -R "write ${TMPDIR}/BROOK.MNT BROOK.MNT" 2>/dev/null
rm -rf "${TMPDIR}"

echo "Ext4 disk image created: ${DISK_IMG}"
echo "  Size:       ${SIZE_MB}MB"
echo "  Mount path: ${MOUNT_PATH}"
echo ""
echo "Populate it with:"
echo "  debugfs -w ${DISK_IMG} -R 'write <src> <dest>'"
echo "  debugfs ${DISK_IMG} -R 'ls /'         # list contents"
