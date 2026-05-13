#!/usr/bin/env bash
# Create a bootable GPT USB disk image for Brook OS.
#
# Layout:
#   Partition 1: EFI System Partition (FAT32, 512MB) — bootloader + kernel + initrd
#   Partition 2: ext2 root filesystem (from brook_ext2_disk.img)
#   Partition 3: ext2 nix store (from brook_nix_disk.img)
#   Partition 4: ext2 home (from brook_home_disk.img)
#
# Usage:
#   scripts/create_usb_image.sh [--size 32G] [--output brook_usb.img]
#
# The resulting image can be dd'd to a USB stick:
#   sudo dd if=brook_usb.img of=/dev/sdX bs=4M status=progress && sync
#
# For incremental updates (fast!), use:
#   scripts/update_usb_image.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${ROOT_DIR}/build/release"

# Defaults
OUTPUT="${BROOK_USB_IMG:-${ROOT_DIR}/brook_usb.img}"
TOTAL_SIZE="8G"    # Actual used size (not full USB — easier to work with)

# Parse args
while [[ $# -gt 0 ]]; do
    case "$1" in
        --size)   TOTAL_SIZE="$2"; shift 2 ;;
        --output) OUTPUT="$2"; shift 2 ;;
        *)        echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# Partition sizes (in MB)
ESP_SIZE_MB=512
ROOT_SIZE_MB=2048
NIX_SIZE_MB=5120
HOME_SIZE_MB=256

# Convert total size to MB
case "${TOTAL_SIZE}" in
    *G) TOTAL_MB=$(( ${TOTAL_SIZE%G} * 1024 )) ;;
    *M) TOTAL_MB=${TOTAL_SIZE%M} ;;
    *)  TOTAL_MB=${TOTAL_SIZE} ;;
esac

echo "=== Brook OS USB Image Creator ==="
echo "Output: ${OUTPUT}"
echo "Size:   ${TOTAL_MB} MB"
echo ""

# Check prerequisites
for tool in sgdisk mkfs.fat mkfs.ext2 mtools mcopy mmd; do
    if ! command -v "$tool" &>/dev/null; then
        echo "ERROR: '$tool' not found. Make sure gptfdisk, dosfstools, e2fsprogs, mtools are available."
        exit 1
    fi
done

# Check build artifacts exist
BOOTLOADER="${BUILD_DIR}/bootloader/BOOTX64.efi"
KERNEL="${BUILD_DIR}/kernel/BROOK.elf"
if [ ! -f "${BOOTLOADER}" ]; then
    echo "ERROR: Bootloader not found at ${BOOTLOADER}"
    echo "Run: scripts/build.sh release"
    exit 1
fi
if [ ! -f "${KERNEL}" ]; then
    echo "ERROR: Kernel not found at ${KERNEL}"
    echo "Run: scripts/build.sh release"
    exit 1
fi

# Create image
echo "Creating ${TOTAL_MB}MB image..."
dd if=/dev/zero of="${OUTPUT}" bs=1M count="${TOTAL_MB}" status=none

# Create GPT partition table
echo "Creating GPT partition table..."
sgdisk --clear "${OUTPUT}" >/dev/null

# Calculate partition boundaries (1MB alignment)
ESP_START=2048             # sector 2048 = 1MB offset
ESP_END=$(( ESP_START + ESP_SIZE_MB * 2048 - 1 ))
ROOT_START=$(( ESP_END + 1 ))
ROOT_END=$(( ROOT_START + ROOT_SIZE_MB * 2048 - 1 ))
NIX_START=$(( ROOT_END + 1 ))
NIX_END=$(( NIX_START + NIX_SIZE_MB * 2048 - 1 ))
HOME_START=$(( NIX_END + 1 ))
HOME_END=$(( HOME_START + HOME_SIZE_MB * 2048 - 1 ))

sgdisk \
    --new=1:${ESP_START}:${ESP_END}   --typecode=1:EF00 --change-name=1:"EFI System" \
    --new=2:${ROOT_START}:${ROOT_END} --typecode=2:8300 --change-name=2:"BROOK_ROOT" \
    --new=3:${NIX_START}:${NIX_END}   --typecode=3:8300 --change-name=3:"BROOK_NIX" \
    --new=4:${HOME_START}:${HOME_END} --typecode=4:8300 --change-name=4:"BROOK_HOME" \
    "${OUTPUT}" >/dev/null

echo "  Partition 1 (ESP):  ${ESP_SIZE_MB} MB"
echo "  Partition 2 (root): ${ROOT_SIZE_MB} MB"
echo "  Partition 3 (nix):  ${NIX_SIZE_MB} MB"
echo "  Partition 4 (home): ${HOME_SIZE_MB} MB"

# Format ESP partition
echo ""
echo "Formatting ESP (FAT32)..."
ESP_OFFSET=$(( ESP_START * 512 ))
ESP_BYTES=$(( ESP_SIZE_MB * 1024 * 1024 ))
# Extract ESP partition to a temp file, format it, write back
ESP_TMP=$(mktemp /tmp/brook-esp-XXXXXX.img)
dd if=/dev/zero of="${ESP_TMP}" bs=1M count=${ESP_SIZE_MB} status=none
mkfs.fat -F 32 -n "BROOK_ESP" "${ESP_TMP}" >/dev/null

# Populate ESP
mmd -i "${ESP_TMP}" ::EFI
mmd -i "${ESP_TMP}" ::EFI/BOOT
mmd -i "${ESP_TMP}" ::KERNEL
mcopy -i "${ESP_TMP}" "${BOOTLOADER}" "::EFI/BOOT/BOOTX64.EFI"
mcopy -i "${ESP_TMP}" "${KERNEL}" "::KERNEL/BROOK.ELF"

# Create BROOK.CFG
BROOK_CFG_TMP=$(mktemp /tmp/brook-cfg-XXXXXX)
cat > "${BROOK_CFG_TMP}" <<'EOF'
# Brook OS boot configuration
TARGET=KERNEL\BROOK.ELF
DEBUG_TEXT=0
LOG_MEMORY=0
LOG_INTERRUPTS=0
EOF
mcopy -i "${ESP_TMP}" "${BROOK_CFG_TMP}" "::BROOK.CFG"
rm -f "${BROOK_CFG_TMP}"

# Copy initrd if present
INITRD="${BUILD_DIR}/kernel/initrd.img"
if [ -f "${INITRD}" ]; then
    mcopy -i "${ESP_TMP}" "${INITRD}" "::KERNEL/INITRD.IMG"
    echo "  initrd: $(du -h "${INITRD}" | cut -f1)"
fi

# Write ESP back into the main image
dd if="${ESP_TMP}" of="${OUTPUT}" bs=512 seek=${ESP_START} conv=notrunc status=none
rm -f "${ESP_TMP}"
echo "  ESP populated with bootloader + kernel"

# Format and populate root partition
echo ""
echo "Formatting root partition (ext2)..."
ROOT_OFFSET_BYTES=$(( ROOT_START * 512 ))
ROOT_BYTES=$(( ROOT_SIZE_MB * 1024 * 1024 ))
ROOT_TMP=$(mktemp /tmp/brook-root-XXXXXX.img)

# If existing ext2 disk exists and fits, copy it directly
EXISTING_ROOT="${BROOK_EXT2_DISK:-${ROOT_DIR}/brook_ext2_disk.img}"
if [ -f "${EXISTING_ROOT}" ]; then
    EXISTING_SIZE=$(stat -c%s "${EXISTING_ROOT}")
    if [ "${EXISTING_SIZE}" -le "${ROOT_BYTES}" ]; then
        echo "  Copying existing root image ($(( EXISTING_SIZE / 1024 / 1024 )) MB)..."
        cp "${EXISTING_ROOT}" "${ROOT_TMP}"
        # Extend to partition size if smaller
        if [ "${EXISTING_SIZE}" -lt "${ROOT_BYTES}" ]; then
            truncate -s "${ROOT_BYTES}" "${ROOT_TMP}"
            resize2fs -f "${ROOT_TMP}" >/dev/null 2>&1 || true
        fi
    else
        echo "  WARNING: existing root (${EXISTING_SIZE}) exceeds partition (${ROOT_BYTES}), creating fresh"
        dd if=/dev/zero of="${ROOT_TMP}" bs=1M count=${ROOT_SIZE_MB} status=none
        mkfs.ext2 -q -b 4096 -L "BROOK_ROOT" "${ROOT_TMP}"
    fi
else
    echo "  No existing root image, creating empty ext2..."
    dd if=/dev/zero of="${ROOT_TMP}" bs=1M count=${ROOT_SIZE_MB} status=none
    mkfs.ext2 -q -b 4096 -L "BROOK_ROOT" "${ROOT_TMP}"
    # Write BROOK.MNT
    TMPDIR=$(mktemp -d)
    echo -n "/data" > "${TMPDIR}/BROOK.MNT"
    debugfs -w "${ROOT_TMP}" -R "write ${TMPDIR}/BROOK.MNT BROOK.MNT" 2>/dev/null
    rm -rf "${TMPDIR}"
fi
dd if="${ROOT_TMP}" of="${OUTPUT}" bs=512 seek=${ROOT_START} conv=notrunc status=none
rm -f "${ROOT_TMP}"

# Format and populate nix partition
echo ""
echo "Formatting nix partition (ext2)..."
NIX_BYTES=$(( NIX_SIZE_MB * 1024 * 1024 ))
NIX_TMP=$(mktemp /tmp/brook-nix-XXXXXX.img)
EXISTING_NIX="${BROOK_NIX_DISK:-${ROOT_DIR}/brook_nix_disk.img}"
if [ -f "${EXISTING_NIX}" ]; then
    EXISTING_SIZE=$(stat -c%s "${EXISTING_NIX}")
    if [ "${EXISTING_SIZE}" -le "${NIX_BYTES}" ]; then
        echo "  Copying existing nix image ($(( EXISTING_SIZE / 1024 / 1024 )) MB)..."
        cp "${EXISTING_NIX}" "${NIX_TMP}"
        if [ "${EXISTING_SIZE}" -lt "${NIX_BYTES}" ]; then
            truncate -s "${NIX_BYTES}" "${NIX_TMP}"
            resize2fs -f "${NIX_TMP}" >/dev/null 2>&1 || true
        fi
    else
        echo "  WARNING: existing nix (${EXISTING_SIZE}) exceeds partition (${NIX_BYTES}), creating fresh"
        dd if=/dev/zero of="${NIX_TMP}" bs=1M count=${NIX_SIZE_MB} status=none
        mkfs.ext2 -q -b 4096 -L "BROOK_NIX" "${NIX_TMP}"
    fi
else
    echo "  No existing nix image, creating empty ext2..."
    dd if=/dev/zero of="${NIX_TMP}" bs=1M count=${NIX_SIZE_MB} status=none
    mkfs.ext2 -q -b 4096 -L "BROOK_NIX" "${NIX_TMP}"
    TMPDIR=$(mktemp -d)
    echo -n "/nix" > "${TMPDIR}/BROOK.MNT"
    debugfs -w "${NIX_TMP}" -R "write ${TMPDIR}/BROOK.MNT BROOK.MNT" 2>/dev/null
    rm -rf "${TMPDIR}"
fi
dd if="${NIX_TMP}" of="${OUTPUT}" bs=512 seek=${NIX_START} conv=notrunc status=none
rm -f "${NIX_TMP}"

# Format and populate home partition
echo ""
echo "Formatting home partition (ext2)..."
HOME_BYTES=$(( HOME_SIZE_MB * 1024 * 1024 ))
HOME_TMP=$(mktemp /tmp/brook-home-XXXXXX.img)
EXISTING_HOME="${BROOK_HOME_DISK:-${ROOT_DIR}/brook_home_disk.img}"
if [ -f "${EXISTING_HOME}" ]; then
    EXISTING_SIZE=$(stat -c%s "${EXISTING_HOME}")
    if [ "${EXISTING_SIZE}" -le "${HOME_BYTES}" ]; then
        echo "  Copying existing home image ($(( EXISTING_SIZE / 1024 / 1024 )) MB)..."
        cp "${EXISTING_HOME}" "${HOME_TMP}"
        if [ "${EXISTING_SIZE}" -lt "${HOME_BYTES}" ]; then
            truncate -s "${HOME_BYTES}" "${HOME_TMP}"
            resize2fs -f "${HOME_TMP}" >/dev/null 2>&1 || true
        fi
    else
        echo "  WARNING: existing home exceeds partition, creating fresh"
        dd if=/dev/zero of="${HOME_TMP}" bs=1M count=${HOME_SIZE_MB} status=none
        mkfs.ext2 -q -b 4096 -L "BROOK_HOME" "${HOME_TMP}"
    fi
else
    echo "  No existing home image, creating empty ext2..."
    dd if=/dev/zero of="${HOME_TMP}" bs=1M count=${HOME_SIZE_MB} status=none
    mkfs.ext2 -q -b 4096 -L "BROOK_HOME" "${HOME_TMP}"
    TMPDIR=$(mktemp -d)
    echo -n "/home" > "${TMPDIR}/BROOK.MNT"
    debugfs -w "${HOME_TMP}" -R "write ${TMPDIR}/BROOK.MNT BROOK.MNT" 2>/dev/null
    rm -rf "${TMPDIR}"
fi
dd if="${HOME_TMP}" of="${OUTPUT}" bs=512 seek=${HOME_START} conv=notrunc status=none
rm -f "${HOME_TMP}"

echo ""
echo "=== USB image created: ${OUTPUT} ==="
echo "Size: $(du -h "${OUTPUT}" | cut -f1)"
echo ""
echo "To write to USB:"
echo "  sudo dd if=${OUTPUT} of=/dev/sdX bs=4M status=progress && sync"
echo ""
echo "To update incrementally:"
echo "  scripts/update_usb_image.sh"
