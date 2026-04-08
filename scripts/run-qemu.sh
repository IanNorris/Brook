#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${ROOT_DIR}/build"
ESP_DIR="${BUILD_DIR}/esp/EFI/BOOT"
BOOTLOADER="${BUILD_DIR}/bootloader/BOOTX64.efi"

if [ ! -f "${BOOTLOADER}" ]; then
    echo "Bootloader not found at ${BOOTLOADER}"
    echo "Run scripts/build.sh first."
    exit 1
fi

# Set up EFI System Partition layout
mkdir -p "${ESP_DIR}"
cp "${BOOTLOADER}" "${ESP_DIR}/BOOTX64.EFI"

# Find OVMF firmware - check common locations
OVMF_CODE=""
OVMF_VARS=""
OVMF_SEARCH_PATHS=(
    # NixOS
    "/run/current-system/sw/share/OVMF"
    "/nix/var/nix/profiles/default/share/OVMF"
    # Debian/Ubuntu
    "/usr/share/OVMF"
    "/usr/share/ovmf"
    # Arch
    "/usr/share/edk2-ovmf/x64"
    "/usr/share/edk2/ovmf"
    # Fedora
    "/usr/share/edk2/ovmf"
)

for dir in "${OVMF_SEARCH_PATHS[@]}"; do
    if [ -f "${dir}/OVMF_CODE.fd" ]; then
        OVMF_CODE="${dir}/OVMF_CODE.fd"
        OVMF_VARS="${dir}/OVMF_VARS.fd"
        break
    elif [ -f "${dir}/OVMF.fd" ]; then
        OVMF_CODE="${dir}/OVMF.fd"
        OVMF_VARS="${dir}/OVMF.fd"
        break
    fi
done

if [ -z "${OVMF_CODE}" ]; then
    echo "Error: OVMF firmware not found."
    echo "On NixOS: nix-shell -p OVMF"
    echo "On Debian: sudo apt install ovmf"
    echo "Or set OVMF_CODE env var to the firmware path."
    exit 1
fi

# Copy OVMF vars to a writable location (UEFI needs writable NVRAM)
OVMF_VARS_COPY="${BUILD_DIR}/OVMF_VARS.fd"
cp "${OVMF_VARS}" "${OVMF_VARS_COPY}"

echo "Starting QEMU..."
echo "  OVMF: ${OVMF_CODE}"
echo "  ESP:  ${BUILD_DIR}/esp"

qemu-system-x86_64 \
    -machine q35 \
    -cpu qemu64 \
    -m 256M \
    -drive if=pflash,format=raw,readonly=on,file="${OVMF_CODE}" \
    -drive if=pflash,format=raw,file="${OVMF_VARS_COPY}" \
    -drive format=raw,file=fat:rw:"${BUILD_DIR}/esp" \
    -serial stdio \
    -display gtk \
    -monitor none \
    "$@"
