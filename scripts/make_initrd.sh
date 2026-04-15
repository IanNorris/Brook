#!/usr/bin/env bash
# make_initrd.sh — Create the INITRD.IMG FAT16 image for the Brook OS ESP.
#
# This image is loaded by the bootloader alongside the kernel and mounted
# as the root ramdisk ("/") before virtio disks are available.
# It contains BROOK.CFG and any early-boot driver modules.
#
# Usage:
#   scripts/make_initrd.sh [--release|--debug]
#
# Output: build/<type>/esp/INITRD.IMG
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

BUILD_TYPE="release"
for arg in "$@"; do
    case "$arg" in
        --debug)   BUILD_TYPE="debug" ;;
        --release) BUILD_TYPE="release" ;;
    esac
done

BUILD_DIR="${ROOT_DIR}/build/${BUILD_TYPE}"
ESP_DIR="${BUILD_DIR}/esp"
INITRD_IMG="${ESP_DIR}/INITRD.IMG"
MOD_DIR="${BUILD_DIR}/kernel/drivers"

# Image size in KB — enough for config + a few modules.
# mformat needs power-of-two-ish sizes; 720KB is a standard floppy format.
SIZE_KB=720

echo "Creating initrd image (${SIZE_KB} KB)..."

# Create FAT16 image
MTOOLSRC=$(mktemp)
trap "rm -f '${MTOOLSRC}' '${INITRD_IMG}.tmp'" EXIT

# Use a temp file, rename atomically at the end
dd if=/dev/zero of="${INITRD_IMG}.tmp" bs=1K count=${SIZE_KB} status=none

# mformat to create FAT16 filesystem
cat > "${MTOOLSRC}" <<EOF
drive z:
    file="${INITRD_IMG}.tmp"
    partition=1
EOF

# mformat -i uses image directly (no mtools.conf drive mapping needed)
mformat -i "${INITRD_IMG}.tmp" -v "INITRD" -f ${SIZE_KB} :: 2>/dev/null || \
    mformat -i "${INITRD_IMG}.tmp" -v "INITRD" -C -T $((SIZE_KB * 2)) -h 2 -s 18 :: 2>/dev/null

# Write BROOK.CFG
echo -e "# Brook OS boot config (initrd)\nTARGET=KERNEL\\\\BROOK.ELF" | \
    mcopy -i "${INITRD_IMG}.tmp" - "::BROOK.CFG"

# Create drivers directory and copy any .mod files
mmd -i "${INITRD_IMG}.tmp" "::DRIVERS" 2>/dev/null || true

if [ -d "${MOD_DIR}" ]; then
    for mod in "${MOD_DIR}"/*.mod; do
        [ -f "$mod" ] || continue
        name="$(basename "$mod" | tr '[:lower:]' '[:upper:]')"
        mcopy -i "${INITRD_IMG}.tmp" "$mod" "::DRIVERS/${name}"
        echo "  packed: DRIVERS/${name} ($(stat -c%s "$mod") bytes)"
    done
fi

# Create TEST directory for smoke tests (like the old embedded image)
mmd -i "${INITRD_IMG}.tmp" "::TEST" 2>/dev/null || true
echo "Hello from initrd!" | mcopy -i "${INITRD_IMG}.tmp" - "::TEST/HELLO.TXT"

# Move into place
mv "${INITRD_IMG}.tmp" "${INITRD_IMG}"

echo "Initrd image created: ${INITRD_IMG} ($(stat -c%s "${INITRD_IMG}") bytes)"
echo ""
mdir -i "${INITRD_IMG}" :: 2>/dev/null || true
