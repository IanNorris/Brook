#!/usr/bin/env bash
# Create a GPT-partitioned disk image for testing the kernel's GPT scanner.
#
# This replaces the separate home + data disks with a single GPT volume:
#   Partition 1: ext2  (home, mounted at /home)
#   Partition 2: FAT32 (data, mounted at /media)
#
# Usage:
#   scripts/create_gpt_disk.sh [size_mb]    # default: 320
#
# The resulting image is used as BROOK_HOME_DISK (virtio drive).
# Set BROOK_GPT_DISK to override the output path.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
DISK_IMG="${BROOK_GPT_DISK:-${ROOT_DIR}/brook_gpt_disk.img}"
SIZE_MB="${1:-320}"

# Partition sizes
HOME_SIZE_MB=192
DATA_SIZE_MB=96

if [ -f "${DISK_IMG}" ]; then
    echo "GPT disk image already exists at ${DISK_IMG}"
    echo "Delete it first if you want to recreate: rm ${DISK_IMG}"
    exit 1
fi

# Check tools
for tool in sgdisk mkfs.ext2 mkfs.fat; do
    if ! command -v "$tool" &>/dev/null; then
        echo "ERROR: '$tool' not found"
        exit 1
    fi
done

echo "Creating ${SIZE_MB}MB GPT disk image at ${DISK_IMG}..."

# Create empty image
dd if=/dev/zero of="${DISK_IMG}" bs=1M count="${SIZE_MB}" status=none

# Create GPT
sgdisk --clear "${DISK_IMG}" >/dev/null

# Partition 1: ext2 home (starting at 2048 sectors = 1MB)
HOME_START=2048
HOME_END=$(( HOME_START + HOME_SIZE_MB * 2048 - 1 ))
# Partition 2: FAT32 data
DATA_START=$(( HOME_END + 1 ))
DATA_END=$(( DATA_START + DATA_SIZE_MB * 2048 - 1 ))

sgdisk \
    --new=1:${HOME_START}:${HOME_END}   --typecode=1:8300 --change-name=1:"BROOK_HOME" \
    --new=2:${DATA_START}:${DATA_END}   --typecode=2:8300 --change-name=2:"BROOK_DATA" \
    "${DISK_IMG}" >/dev/null

echo "  Partition 1 (home): ${HOME_SIZE_MB} MB (ext2)"
echo "  Partition 2 (data): ${DATA_SIZE_MB} MB (FAT32)"

# Format partition 1 as ext2
HOME_TMP=$(mktemp /tmp/brook-gpt-home-XXXXXX.img)
dd if=/dev/zero of="${HOME_TMP}" bs=1M count=${HOME_SIZE_MB} status=none
mkfs.ext2 -q -b 4096 -L "BROOK_HOME" "${HOME_TMP}"

# Write BROOK.MNT
TMPDIR=$(mktemp -d)
echo -n "/home" > "${TMPDIR}/BROOK.MNT"
debugfs -w "${HOME_TMP}" -R "write ${TMPDIR}/BROOK.MNT BROOK.MNT" 2>/dev/null

# If existing home disk has content, copy it over
EXISTING_HOME="${BROOK_HOME_DISK:-${ROOT_DIR}/brook_home_disk.img}"
if [ -f "${EXISTING_HOME}" ] && command -v fuse2fs &>/dev/null; then
    echo "  Copying existing home content..."
    SRC_MNT=$(mktemp -d)
    DST_MNT=$(mktemp -d)
    fuse2fs -o ro,fakeroot "${EXISTING_HOME}" "${SRC_MNT}" 2>/dev/null || true
    fuse2fs -o rw,fakeroot "${HOME_TMP}" "${DST_MNT}" 2>/dev/null || true
    if [ -d "${SRC_MNT}/lost+found" ]; then
        cp -a "${SRC_MNT}"/* "${DST_MNT}/" 2>/dev/null || true
        sync
    fi
    fusermount -u "${DST_MNT}" 2>/dev/null || true
    fusermount -u "${SRC_MNT}" 2>/dev/null || true
    rmdir "${SRC_MNT}" "${DST_MNT}" 2>/dev/null || true
fi
rm -rf "${TMPDIR}"

# Write home partition into disk image
dd if="${HOME_TMP}" of="${DISK_IMG}" bs=512 seek=${HOME_START} conv=notrunc status=none
rm -f "${HOME_TMP}"

# Format partition 2 as FAT32
DATA_TMP=$(mktemp /tmp/brook-gpt-data-XXXXXX.img)
dd if=/dev/zero of="${DATA_TMP}" bs=1M count=${DATA_SIZE_MB} status=none
mkfs.fat -F 32 -n "BROOK_DATA" "${DATA_TMP}" >/dev/null

# Write BROOK.MNT
echo -n "/media" | mcopy -i "${DATA_TMP}" - "::BROOK.MNT"
mmd -i "${DATA_TMP}" ::MUSIC 2>/dev/null || true
mmd -i "${DATA_TMP}" ::VIDEOS 2>/dev/null || true

# Write data partition into disk image
dd if="${DATA_TMP}" of="${DISK_IMG}" bs=512 seek=${DATA_START} conv=notrunc status=none
rm -f "${DATA_TMP}"

echo ""
echo "GPT disk image created: ${DISK_IMG}"
echo ""
echo "To use with QEMU (replaces separate home disk):"
echo "  export BROOK_HOME_DISK=${DISK_IMG}"
echo "  scripts/run-qemu.sh --release"
echo ""
echo "Verify with: sgdisk -p ${DISK_IMG}"
sgdisk -p "${DISK_IMG}" 2>/dev/null || true
