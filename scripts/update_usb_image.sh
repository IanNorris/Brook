#!/usr/bin/env bash
# Incrementally update a Brook OS USB image (or physical USB stick).
#
# This script updates ONLY changed files — typically takes seconds, not minutes.
# Works on both the .img file and a mounted USB device.
#
# Usage:
#   scripts/update_usb_image.sh [--device /dev/sdX] [--image brook_usb.img]
#   scripts/update_usb_image.sh --esp-only       # only update bootloader/kernel
#   scripts/update_usb_image.sh --verify         # verify partition integrity
#
# What gets updated:
#   ESP:  bootloader, kernel, initrd, BROOK.CFG
#   Root: synced from brook_ext2_disk.img (rsync via fuse2fs)
#   Nix:  synced from brook_nix_disk.img
#   Home: synced from brook_home_disk.img
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${ROOT_DIR}/build/release"

# Defaults
TARGET=""
DEVICE=""
ESP_ONLY=0
VERIFY_ONLY=0

# Parse args
while [[ $# -gt 0 ]]; do
    case "$1" in
        --device)    DEVICE="$2"; shift 2 ;;
        --image)     TARGET="$2"; shift 2 ;;
        --esp-only)  ESP_ONLY=1; shift ;;
        --verify)    VERIFY_ONLY=1; shift ;;
        *)           echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# Determine target
if [ -n "${DEVICE}" ]; then
    # Working directly on a USB device
    echo "=== Updating USB device: ${DEVICE} ==="
    if [ ! -b "${DEVICE}" ]; then
        echo "ERROR: ${DEVICE} is not a block device"
        exit 1
    fi
    # Partitions are ${DEVICE}1, ${DEVICE}2, etc. (or ${DEVICE}p1 for nvme)
    if [ -b "${DEVICE}1" ]; then
        PART_PREFIX="${DEVICE}"
    elif [ -b "${DEVICE}p1" ]; then
        PART_PREFIX="${DEVICE}p"
    else
        echo "ERROR: Cannot find partitions on ${DEVICE}"
        exit 1
    fi
    ESP_PART="${PART_PREFIX}1"
    ROOT_PART="${PART_PREFIX}2"
    NIX_PART="${PART_PREFIX}3"
    HOME_PART="${PART_PREFIX}4"
    MODE="device"
else
    TARGET="${TARGET:-${BROOK_USB_IMG:-${ROOT_DIR}/brook_usb.img}}"
    if [ ! -f "${TARGET}" ]; then
        echo "ERROR: USB image not found at ${TARGET}"
        echo "Run: scripts/create_usb_image.sh"
        exit 1
    fi
    echo "=== Updating USB image: ${TARGET} ==="
    MODE="image"

    # Read partition offsets from GPT using sgdisk
    # Format: start_sector:end_sector for each partition
    ESP_START=$(sgdisk -i 1 "${TARGET}" 2>/dev/null | grep "First sector" | awk '{print $3}')
    ROOT_START=$(sgdisk -i 2 "${TARGET}" 2>/dev/null | grep "First sector" | awk '{print $3}')
    NIX_START=$(sgdisk -i 3 "${TARGET}" 2>/dev/null | grep "First sector" | awk '{print $3}')
    HOME_START=$(sgdisk -i 4 "${TARGET}" 2>/dev/null | grep "First sector" | awk '{print $3}')
    ESP_END=$(sgdisk -i 1 "${TARGET}" 2>/dev/null | grep "Last sector" | awk '{print $3}')
    ROOT_END=$(sgdisk -i 2 "${TARGET}" 2>/dev/null | grep "Last sector" | awk '{print $3}')
    NIX_END=$(sgdisk -i 3 "${TARGET}" 2>/dev/null | grep "Last sector" | awk '{print $3}')
    HOME_END=$(sgdisk -i 4 "${TARGET}" 2>/dev/null | grep "Last sector" | awk '{print $3}')
fi

# --- Helper: extract a partition from the image to a temp file ---
extract_partition() {
    local start_sector=$1
    local end_sector=$2
    local tmpfile=$3
    local size_sectors=$(( end_sector - start_sector + 1 ))
    dd if="${TARGET}" of="${tmpfile}" bs=512 skip="${start_sector}" count="${size_sectors}" status=none
}

# --- Helper: write a partition back to the image ---
write_partition() {
    local start_sector=$1
    local tmpfile=$2
    dd if="${tmpfile}" of="${TARGET}" bs=512 seek="${start_sector}" conv=notrunc status=none
}

# --- Update ESP ---
update_esp() {
    echo ""
    echo "--- Updating ESP (bootloader + kernel) ---"
    local BOOTLOADER="${BUILD_DIR}/bootloader/BOOTX64.efi"
    local KERNEL="${BUILD_DIR}/kernel/BROOK.elf"
    local INITRD="${BUILD_DIR}/kernel/initrd.img"

    if [ "${MODE}" = "image" ]; then
        local ESP_TMP=$(mktemp /tmp/brook-esp-update-XXXXXX.img)
        extract_partition "${ESP_START}" "${ESP_END}" "${ESP_TMP}"

        # Update files using mcopy -o (overwrite)
        [ -f "${BOOTLOADER}" ] && mcopy -o -i "${ESP_TMP}" "${BOOTLOADER}" "::EFI/BOOT/BOOTX64.EFI" && echo "  ✓ BOOTX64.EFI"
        [ -f "${KERNEL}" ] && mcopy -o -i "${ESP_TMP}" "${KERNEL}" "::KERNEL/BROOK.ELF" && echo "  ✓ BROOK.ELF"
        [ -f "${INITRD}" ] && mcopy -o -i "${ESP_TMP}" "${INITRD}" "::KERNEL/INITRD.IMG" && echo "  ✓ INITRD.IMG"

        write_partition "${ESP_START}" "${ESP_TMP}"
        rm -f "${ESP_TMP}"
    else
        # Direct device mode — mount the ESP
        local MNT=$(mktemp -d)
        mount "${ESP_PART}" "${MNT}"
        [ -f "${BOOTLOADER}" ] && cp "${BOOTLOADER}" "${MNT}/EFI/BOOT/BOOTX64.EFI" && echo "  ✓ BOOTX64.EFI"
        [ -f "${KERNEL}" ] && cp "${KERNEL}" "${MNT}/KERNEL/BROOK.ELF" && echo "  ✓ BROOK.ELF"
        [ -f "${INITRD}" ] && cp "${INITRD}" "${MNT}/KERNEL/INITRD.IMG" && echo "  ✓ INITRD.IMG"
        sync
        umount "${MNT}"
        rmdir "${MNT}"
    fi
}

# --- Update an ext2 partition by syncing from source image ---
update_ext2_partition() {
    local part_name=$1
    local source_img=$2
    local part_start=$3
    local part_end=$4
    local part_dev=$5  # only for device mode

    echo ""
    echo "--- Updating ${part_name} ---"

    if [ ! -f "${source_img}" ]; then
        echo "  SKIP: source image not found (${source_img})"
        return
    fi

    if [ "${MODE}" = "image" ]; then
        # Extract partition, then use fuse2fs to do rsync-style copy
        local PART_TMP=$(mktemp /tmp/brook-${part_name}-XXXXXX.img)
        local PART_SIZE_SECTORS=$(( part_end - part_start + 1 ))
        local PART_SIZE_BYTES=$(( PART_SIZE_SECTORS * 512 ))
        local SOURCE_SIZE=$(stat -c%s "${source_img}")

        if [ "${SOURCE_SIZE}" -le "${PART_SIZE_BYTES}" ]; then
            # Source fits — copy it directly (fastest path)
            cp "${source_img}" "${PART_TMP}"
            if [ "${SOURCE_SIZE}" -lt "${PART_SIZE_BYTES}" ]; then
                truncate -s "${PART_SIZE_BYTES}" "${PART_TMP}"
            fi
            write_partition "${part_start}" "${PART_TMP}"
            echo "  ✓ copied from source ($(( SOURCE_SIZE / 1024 / 1024 )) MB)"
        else
            echo "  WARNING: source (${SOURCE_SIZE}) exceeds partition (${PART_SIZE_BYTES})"
        fi
        rm -f "${PART_TMP}"
    else
        # Device mode: use fuse2fs for both source and target, rsync between them
        local SRC_MNT=$(mktemp -d)
        local DST_MNT=$(mktemp -d)
        fuse2fs -o ro,fakeroot "${source_img}" "${SRC_MNT}"
        fuse2fs -o rw,fakeroot "${part_dev}" "${DST_MNT}"
        rsync -a --delete "${SRC_MNT}/" "${DST_MNT}/"
        sync
        fusermount -u "${DST_MNT}"
        fusermount -u "${SRC_MNT}"
        rmdir "${SRC_MNT}" "${DST_MNT}"
        echo "  ✓ synced via rsync"
    fi
}

# --- Verify partition integrity ---
verify_partitions() {
    echo ""
    echo "--- Verifying partition integrity ---"

    if [ "${MODE}" = "image" ]; then
        for part_num in 1 2 3 4; do
            local start end
            start=$(sgdisk -i ${part_num} "${TARGET}" 2>/dev/null | grep "First sector" | awk '{print $3}')
            end=$(sgdisk -i ${part_num} "${TARGET}" 2>/dev/null | grep "Last sector" | awk '{print $3}')
            local name
            name=$(sgdisk -i ${part_num} "${TARGET}" 2>/dev/null | grep "Partition name" | sed "s/.*'\\(.*\\)'/\\1/")

            local TMP=$(mktemp /tmp/brook-verify-XXXXXX.img)
            extract_partition "${start}" "${end}" "${TMP}"

            if [ "${part_num}" -eq 1 ]; then
                # FAT32 — check with mdir
                if mdir -i "${TMP}" :: >/dev/null 2>&1; then
                    echo "  ✓ Partition ${part_num} (${name}): FAT32 OK"
                else
                    echo "  ✗ Partition ${part_num} (${name}): FAT32 CORRUPTED"
                fi
            else
                # ext2 — check with e2fsck
                if e2fsck -n -f "${TMP}" >/dev/null 2>&1; then
                    echo "  ✓ Partition ${part_num} (${name}): ext2 OK"
                else
                    echo "  ✗ Partition ${part_num} (${name}): ext2 ERRORS"
                fi
            fi
            rm -f "${TMP}"
        done
    else
        e2fsck -n -f "${ROOT_PART}" >/dev/null 2>&1 && echo "  ✓ root: ext2 OK" || echo "  ✗ root: ext2 ERRORS"
        e2fsck -n -f "${NIX_PART}" >/dev/null 2>&1  && echo "  ✓ nix: ext2 OK"  || echo "  ✗ nix: ext2 ERRORS"
        e2fsck -n -f "${HOME_PART}" >/dev/null 2>&1 && echo "  ✓ home: ext2 OK" || echo "  ✗ home: ext2 ERRORS"
    fi
}

# --- Main ---
if [ "${VERIFY_ONLY}" -eq 1 ]; then
    verify_partitions
    exit 0
fi

update_esp

if [ "${ESP_ONLY}" -eq 0 ]; then
    EXISTING_ROOT="${BROOK_EXT2_DISK:-${ROOT_DIR}/brook_ext2_disk.img}"
    EXISTING_NIX="${BROOK_NIX_DISK:-${ROOT_DIR}/brook_nix_disk.img}"
    EXISTING_HOME="${BROOK_HOME_DISK:-${ROOT_DIR}/brook_home_disk.img}"

    if [ "${MODE}" = "image" ]; then
        update_ext2_partition "root" "${EXISTING_ROOT}" "${ROOT_START}" "${ROOT_END}" ""
        update_ext2_partition "nix"  "${EXISTING_NIX}"  "${NIX_START}"  "${NIX_END}"  ""
        update_ext2_partition "home" "${EXISTING_HOME}" "${HOME_START}" "${HOME_END}" ""
    else
        update_ext2_partition "root" "${EXISTING_ROOT}" "" "" "${ROOT_PART}"
        update_ext2_partition "nix"  "${EXISTING_NIX}"  "" "" "${NIX_PART}"
        update_ext2_partition "home" "${EXISTING_HOME}" "" "" "${HOME_PART}"
    fi
fi

verify_partitions

echo ""
echo "=== Update complete ==="
