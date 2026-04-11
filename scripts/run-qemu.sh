#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

# Parse arguments
BUILD_TYPE="debug"
DEBUG_FLAGS=""
EXTRA_ARGS=()
for arg in "$@"; do
    case "$arg" in
        --release)
            BUILD_TYPE="release"
            ;;
        --debug)
            DEBUG_FLAGS="-s"
            ;;
        *)
            EXTRA_ARGS+=("$arg")
            ;;
    esac
done

BUILD_DIR="${ROOT_DIR}/build/${BUILD_TYPE}"
ESP_DIR="${BUILD_DIR}/esp/EFI/BOOT"
BOOTLOADER="${BUILD_DIR}/bootloader/BOOTX64.efi"

if [ ! -f "${BOOTLOADER}" ]; then
    echo "Bootloader not found at ${BOOTLOADER}"
    echo "Run: scripts/build.sh $(echo "${BUILD_TYPE}" | sed 's/./\U&/') first."
    exit 1
fi

# Set up EFI System Partition layout
mkdir -p "${ESP_DIR}"
cp "${BOOTLOADER}" "${ESP_DIR}/BOOTX64.EFI"

KERNEL_ELF="${BUILD_DIR}/kernel/BROOK.elf"
KERNEL_ESP_DIR="${BUILD_DIR}/esp/KERNEL"

if [ ! -f "${KERNEL_ELF}" ]; then
    echo "Warning: kernel not found at ${KERNEL_ELF}, running without kernel"
else
    mkdir -p "${KERNEL_ESP_DIR}"
    cp "${KERNEL_ELF}" "${KERNEL_ESP_DIR}/BROOK.ELF"
fi

# Write default BROOK.CFG if not already present
BROOK_CFG="${BUILD_DIR}/esp/BROOK.CFG"
if [ ! -f "${BROOK_CFG}" ]; then
    cat > "${BROOK_CFG}" <<'EOF'
# Brook OS boot configuration
TARGET=KERNEL\BROOK.ELF
DEBUG_TEXT=0
LOG_MEMORY=0
LOG_INTERRUPTS=0
EOF
fi

# Find OVMF firmware - check env vars first (set by shell.nix), then common paths
OVMF_CODE="${OVMF_CODE:-}"
OVMF_VARS="${OVMF_VARS:-}"

if [ -z "${OVMF_CODE}" ]; then
    OVMF_SEARCH_PATHS=(
        # NixOS system profile (if OVMF added to environment.systemPackages)
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
fi

# NixOS fallback: ask Nix where OVMF lives in the store
if [ -z "${OVMF_CODE}" ] && command -v nix-build &>/dev/null; then
    echo "Locating OVMF via nix-build..."
    OVMF_STORE="$(nix-build '<nixpkgs>' -A OVMF.fd --no-out-link 2>/dev/null || true)"
    if [ -n "${OVMF_STORE}" ] && [ -f "${OVMF_STORE}/FV/OVMF_CODE.fd" ]; then
        OVMF_CODE="${OVMF_STORE}/FV/OVMF_CODE.fd"
        OVMF_VARS="${OVMF_STORE}/FV/OVMF_VARS.fd"
    fi
fi

if [ -z "${OVMF_CODE}" ]; then
    echo "Error: OVMF firmware not found."
    echo "Easiest fix — run from the repo's nix-shell which sets \$OVMF_CODE automatically:"
    echo "  cd $(dirname "$SCRIPT_DIR") && nix-shell && ./scripts/run-qemu.sh"
    echo ""
    echo "Or set manually: export OVMF_CODE=/path/to/OVMF_CODE.fd"
    exit 1
fi

# Copy OVMF vars to a writable location (UEFI needs writable NVRAM for boot entries etc.)
# Use /tmp so we don't need write access to the build directory just to start QEMU.
OVMF_VARS_COPY="$(mktemp /tmp/brook-OVMF_VARS-XXXXXX.fd)"
cp "${OVMF_VARS}" "${OVMF_VARS_COPY}"
trap 'rm -f "${OVMF_VARS_COPY}"' EXIT

echo "Starting QEMU (${BUILD_TYPE})..."
echo "  OVMF: ${OVMF_CODE}"
echo "  ESP:  ${BUILD_DIR}/esp"

if [ -n "${DEBUG_FLAGS}" ]; then
    echo "  GDB:  listening on localhost:1234 (run scripts/gdb-kernel.sh in another terminal)"
fi

# Use persistent disk image (shared between debug/release).
# Create it automatically on first run if missing.
DISK_IMG="${BROOK_DISK_IMG:-${ROOT_DIR}/brook_disk.img}"
if [ ! -f "${DISK_IMG}" ]; then
    echo "Creating persistent disk image..."
    "${SCRIPT_DIR}/create_disk.sh"
    "${SCRIPT_DIR}/update_disk.sh"
fi

qemu-system-x86_64 \
    -machine q35 \
    -cpu qemu64 \
    -smp 8 \
    -m 1G \
    -drive if=pflash,format=raw,readonly=on,file="${OVMF_CODE}" \
    -drive if=pflash,format=raw,file="${OVMF_VARS_COPY}" \
    -drive format=raw,file=fat:rw:"${BUILD_DIR}/esp" \
    -drive if=virtio,format=raw,file="${DISK_IMG}" \
    -serial stdio \
    -display gtk \
    -monitor none \
    ${DEBUG_FLAGS} \
    "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
