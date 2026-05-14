#!/usr/bin/env bash
# Quick headless boot test — verifies Brook boots to completion in QEMU.
# Usage: scripts/test-boot.sh [--release|--debug] [--timeout SECONDS]
# Requires: nix-shell (or mkfs.vfat, mtools, qemu in PATH)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_TYPE="release"
TIMEOUT=30

for arg in "$@"; do
    case "$arg" in
        --release) BUILD_TYPE="release" ;;
        --debug)   BUILD_TYPE="debug" ;;
        --timeout=*) TIMEOUT="${arg#--timeout=}" ;;
    esac
done

BUILD_DIR="${ROOT_DIR}/build/${BUILD_TYPE}"
ESP_DIR="${BUILD_DIR}/esp"

if [ ! -f "${ESP_DIR}/EFI/BOOT/BOOTX64.EFI" ]; then
    echo "FAIL: ESP not built (run scripts/build.sh first)"
    exit 1
fi

# Create real FAT32 ESP image
ESP_IMG="$(mktemp /tmp/brook-test-esp-XXXXXX.img)"
SERIAL_LOG="$(mktemp /tmp/brook-test-serial-XXXXXX.log)"
OVMF_VARS_COPY="$(mktemp /tmp/brook-test-nvram-XXXXXX.fd)"
cleanup() { rm -f "${ESP_IMG}" "${SERIAL_LOG}" "${OVMF_VARS_COPY}"; }
trap cleanup EXIT

dd if=/dev/zero of="${ESP_IMG}" bs=1M count=64 2>/dev/null
mkfs.vfat -F 32 "${ESP_IMG}" >/dev/null
(cd "${ESP_DIR}" && find . -type d | while read -r d; do
    [ "$d" = "." ] && continue
    mmd -i "${ESP_IMG}" "::${d#.}" 2>/dev/null || true
done
find . -type f | while read -r f; do
    mcopy -i "${ESP_IMG}" "$f" "::${f#.}"
done)

# Locate OVMF
OVMF_CODE="${OVMF_CODE:-}"
if [ -z "${OVMF_CODE}" ]; then
    echo "FAIL: OVMF_CODE not set (run from nix-shell)"
    exit 1
fi
OVMF_VARS_SRC="${OVMF_VARS:-$(dirname "${OVMF_CODE}")/OVMF_VARS.fd}"
cp "${OVMF_VARS_SRC}" "${OVMF_VARS_COPY}"

# Check required disk images exist
DISK_IMG="${ROOT_DIR}/brook_disk.img"
if [ ! -f "${DISK_IMG}" ]; then
    echo "FAIL: brook_disk.img not found"
    exit 1
fi

# Build QEMU args (only add disk images that exist)
EXTRA_DRIVES=""
for img in brook_ext2_disk.img brook_nix_disk.img brook_home_disk.img; do
    [ -f "${ROOT_DIR}/${img}" ] && EXTRA_DRIVES="${EXTRA_DRIVES} -drive if=virtio,format=raw,file=${ROOT_DIR}/${img},file.locking=off"
done

# Boot
qemu-system-x86_64 \
    -machine q35 \
    -enable-kvm -cpu host \
    -smp 4 \
    -m 4G \
    -drive if=pflash,format=raw,readonly=on,file="${OVMF_CODE}" \
    -drive if=pflash,format=raw,file="${OVMF_VARS_COPY}" \
    -drive if=ide,format=raw,file="${ESP_IMG}" \
    -drive if=virtio,format=raw,file="${DISK_IMG}",file.locking=off \
    ${EXTRA_DRIVES} \
    -device virtio-tablet-pci \
    -device virtio-rng-pci \
    -device virtio-net-pci,netdev=net0 \
    -netdev user,id=net0 \
    -display none \
    -serial file:"${SERIAL_LOG}" \
    -no-reboot \
    -no-shutdown \
    -audiodev none,id=hda0 \
    -daemonize

# Wait for boot completion
echo -n "Waiting for boot..."
ELAPSED=0
while [ $ELAPSED -lt $TIMEOUT ]; do
    if grep -q "BOOT: complete" "${SERIAL_LOG}" 2>/dev/null; then
        echo " OK (${ELAPSED}s)"
        # Check for panics
        if grep -qi "KERNEL PANIC\|TRIPLE FAULT\|ASSERTION FAILED" "${SERIAL_LOG}"; then
            echo "FAIL: kernel panic detected"
            grep -i "panic\|triple\|assert" "${SERIAL_LOG}"
            exit 1
        fi
        echo "PASS: Brook booted successfully"
        # Print summary
        grep "PMM:" "${SERIAL_LOG}" | head -1
        grep "SMP:" "${SERIAL_LOG}" | tail -1
        grep "COMPOSITOR" "${SERIAL_LOG}" | head -1
        # Stop QEMU via monitor or signal
        "${SCRIPT_DIR}/stop-qemu.sh" 2>/dev/null || true
        exit 0
    fi
    sleep 1
    ELAPSED=$((ELAPSED + 1))
    [ $((ELAPSED % 5)) -eq 0 ] && echo -n " ${ELAPSED}s"
done

echo " TIMEOUT (${TIMEOUT}s)"
echo "Last 20 lines of serial output:"
tail -20 "${SERIAL_LOG}"
"${SCRIPT_DIR}/stop-qemu.sh" 2>/dev/null || true
exit 1
