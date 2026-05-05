#!/usr/bin/env bash
# Create a data disk image for large media files (music, videos).
#
# This creates an ext2 image with BROOK.MNT="/media" so the kernel
# mounts it at /media. Use this instead of putting large files on
# the boot FAT disk.
#
# Usage:
#   scripts/create_data_disk.sh [size_mb] [file1 file2 ...]
#
# Examples:
#   scripts/create_data_disk.sh 256
#   scripts/create_data_disk.sh 512 ~/Videos/sample.mp4 ~/Music/song.mp3
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
DISK_IMG="${BROOK_DATA_DISK:-${ROOT_DIR}/brook_data_disk.img}"
SIZE_MB="${1:-256}"
shift 2>/dev/null || true

if [ -f "${DISK_IMG}" ]; then
    echo "Data disk already exists at ${DISK_IMG}"
    echo "Delete it first if you want to recreate: rm ${DISK_IMG}"
    echo "Or use --add to add files to existing disk."
    if [ "${1:-}" = "--add" ]; then
        shift
    else
        exit 1
    fi
fi

if [ ! -f "${DISK_IMG}" ]; then
    echo "Creating ${SIZE_MB}MB data disk at ${DISK_IMG}..."

    dd if=/dev/zero of="${DISK_IMG}" bs=1M count="${SIZE_MB}" status=none
    mkfs.ext2 -q -b 4096 -L "BROOKDATA" "${DISK_IMG}"

    # Create directory structure and mount point marker
    debugfs -w "${DISK_IMG}" <<'EOF'
mkdir VIDEOS
mkdir MUSIC
EOF

    # Write BROOK.MNT so kernel mounts at /media
    TMPDIR=$(mktemp -d)
    echo -n "/media" > "${TMPDIR}/BROOK.MNT"
    debugfs -w "${DISK_IMG}" -R "write ${TMPDIR}/BROOK.MNT BROOK.MNT" 2>/dev/null
    rm -rf "${TMPDIR}"

    echo "Data disk created: ${DISK_IMG}"
fi

# Copy any specified files into the disk
for FILE in "$@"; do
    if [ ! -f "$FILE" ]; then
        echo "  WARNING: $FILE not found, skipping"
        continue
    fi

    BASENAME="$(basename "$FILE")"
    EXT="${BASENAME##*.}"

    # Determine destination directory based on extension
    case "${EXT,,}" in
        mp4|avi|mkv|webm|mov|flv|wmv)
            DEST="VIDEOS/${BASENAME}"
            ;;
        mp3|wav|flac|ogg|m4a|aac|opus)
            DEST="MUSIC/${BASENAME}"
            ;;
        *)
            DEST="${BASENAME}"
            ;;
    esac

    echo "  Adding: ${FILE} -> ${DEST}"
    debugfs -w "${DISK_IMG}" -R "write ${FILE} ${DEST}" 2>/dev/null
done

echo ""
echo "To use: the data disk is auto-detected by run-qemu.sh"
echo "  BROOK_DATA_DISK=${DISK_IMG}"
echo "Files will be available at /media/VIDEOS/ and /media/MUSIC/ in Brook."
