#!/usr/bin/env bash
# Create an ext2 Brook OS disk image.
#
# Usage:
#   scripts/create_ext2_disk.sh [size_mb]    # default: 128
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
DISK_IMG="${BROOK_EXT2_DISK:-${ROOT_DIR}/brook_ext2_disk.img}"
SIZE_MB="${1:-256}"

if [ -f "${DISK_IMG}" ]; then
    echo "Disk image already exists at ${DISK_IMG}"
    echo "Delete it first if you want to recreate: rm ${DISK_IMG}"
    exit 1
fi

echo "Creating ${SIZE_MB}MB ext2 disk image at ${DISK_IMG}..."

# Create empty image
dd if=/dev/zero of="${DISK_IMG}" bs=1M count="${SIZE_MB}" status=none

# Format as ext2 (no journal — simpler than ext3/4)
mkfs.ext2 -q -b 4096 -L "BROOKDISK" "${DISK_IMG}"

# Create directory structure and BROOK.MNT using debugfs
debugfs -w "${DISK_IMG}" <<'EOF'
mkdir drivers
mkdir bin
EOF

# Write BROOK.MNT (tells kernel to mount at /boot)
TMPDIR=$(mktemp -d)
echo -n "/boot" > "${TMPDIR}/BROOK.MNT"
debugfs -w "${DISK_IMG}" -R "write ${TMPDIR}/BROOK.MNT BROOK.MNT" 2>/dev/null
rm -rf "${TMPDIR}"

echo "Ext2 disk image created: ${DISK_IMG}"
echo ""
echo "Populate it with:"
echo "  scripts/update_ext2_disk.sh           # sync build artifacts"
echo "  debugfs -w ${DISK_IMG} -R 'write <src> <dest>'"
echo "  debugfs ${DISK_IMG} -R 'ls /'         # list contents"
