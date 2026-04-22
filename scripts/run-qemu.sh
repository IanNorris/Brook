#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

# Parse arguments
BUILD_TYPE="debug"
DEBUG_FLAGS=""
SCRIPT_NAME=""
HEADLESS=0
VNC_DISPLAY=""
EXTRA_ARGS=()
for arg in "$@"; do
    case "$arg" in
        --release)
            BUILD_TYPE="release"
            ;;
        --debug)
            DEBUG_FLAGS="-s"
            ;;
        --headless)
            HEADLESS=1
            ;;
        --vnc)
            VNC_DISPLAY="__NEXT_VNC__"
            ;;
        --script=*)
            SCRIPT_NAME="${arg#--script=}"
            ;;
        --script)
            SCRIPT_NAME="__NEXT__"
            ;;
        *)
            if [ "$SCRIPT_NAME" = "__NEXT__" ]; then
                SCRIPT_NAME="$arg"
            elif [ "$VNC_DISPLAY" = "__NEXT_VNC__" ]; then
                VNC_DISPLAY="$arg"
            else
                EXTRA_ARGS+=("$arg")
            fi
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

# Build initrd image (contains BROOK.CFG + early driver modules)
"${SCRIPT_DIR}/make_initrd.sh" "--${BUILD_TYPE}"

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
fi
# Always sync latest files to disk
"${SCRIPT_DIR}/update_disk.sh" "--${BUILD_TYPE}"

# Optional ext2 disk image (added as second virtio drive)
EXT2_DISK="${BROOK_EXT2_DISK:-${ROOT_DIR}/brook_ext2_disk.img}"
EXT2_DRIVE=""
if [ -f "${EXT2_DISK}" ]; then
    EXT2_DRIVE="-drive if=virtio,format=raw,file=${EXT2_DISK},file.locking=off"
    echo "  Ext2 disk: ${EXT2_DISK}"
fi

# Optional Nix store disk (third virtio drive, mounted at /nix)
NIX_DISK="${BROOK_NIX_DISK:-${ROOT_DIR}/brook_nix_disk.img}"
NIX_DRIVE=""
if [ -f "${NIX_DISK}" ]; then
    NIX_DRIVE="-drive if=virtio,format=raw,file=${NIX_DISK},file.locking=off"
    echo "  Nix disk:  ${NIX_DISK}"
fi

HOME_DISK="${BROOK_HOME_DISK:-${ROOT_DIR}/brook_home_disk.img}"
HOME_DRIVE=""
if [ -f "${HOME_DISK}" ]; then
    HOME_DRIVE="-drive if=virtio,format=raw,file=${HOME_DISK},file.locking=off"
    echo "  Home disk: ${HOME_DISK}"
fi

# Select boot script: --script <name> copies data/scripts/<name>.rc to INIT.RC
# on the disk image. Without --script, the existing INIT.RC is used.
if [ -n "${SCRIPT_NAME}" ]; then
    # Try exact path first, then data/scripts/<name>.rc, then data/scripts/<name>
    if [ -f "${SCRIPT_NAME}" ]; then
        SCRIPT_FILE="${SCRIPT_NAME}"
    elif [ -f "${ROOT_DIR}/data/scripts/${SCRIPT_NAME}.rc" ]; then
        SCRIPT_FILE="${ROOT_DIR}/data/scripts/${SCRIPT_NAME}.rc"
    elif [ -f "${ROOT_DIR}/data/scripts/${SCRIPT_NAME}" ]; then
        SCRIPT_FILE="${ROOT_DIR}/data/scripts/${SCRIPT_NAME}"
    else
        echo "Error: boot script '${SCRIPT_NAME}' not found."
        echo "Available scripts:"
        ls "${ROOT_DIR}/data/scripts/"*.rc 2>/dev/null | while read -r f; do
            basename "$f" .rc
        done
        exit 1
    fi
    echo "  Boot script: ${SCRIPT_FILE}"
    mcopy -o -i "${DISK_IMG}" "${SCRIPT_FILE}" "::INIT.RC"
fi

if [ "$HEADLESS" -eq 1 ]; then
    SERIAL_OPT="${SERIAL_OPT:--serial stdio}"
    BROOK_AUDIODEV="${BROOK_AUDIODEV:-none}"
    if [ -n "$VNC_DISPLAY" ] && [ "$VNC_DISPLAY" != "__NEXT_VNC__" ]; then
        DISPLAY_OPT="-vnc ${VNC_DISPLAY}"
    else
        DISPLAY_OPT="-vnc none"
    fi
else
    SERIAL_OPT="-serial stdio"
    DISPLAY_OPT="-display gtk"
fi

KVM_FLAGS=""
if [ -e /dev/kvm ] && [ -r /dev/kvm ] && [ -w /dev/kvm ] && [ "${NO_KVM:-}" != "1" ]; then
    KVM_FLAGS="-enable-kvm -cpu host"
else
    KVM_FLAGS="-cpu qemu64"
fi

qemu-system-x86_64 \
    -machine q35 \
    ${KVM_FLAGS} \
    -smp 8 \
    -m 4G \
    -drive if=pflash,format=raw,readonly=on,file="${OVMF_CODE}" \
    -drive if=pflash,format=raw,file="${OVMF_VARS_COPY}" \
    -drive format=raw,file=fat:rw:"${BUILD_DIR}/esp" \
    -drive if=virtio,format=raw,file="${DISK_IMG}",file.locking=off \
    ${EXT2_DRIVE} \
    ${NIX_DRIVE} \
    ${HOME_DRIVE} \
    -device virtio-tablet-pci \
    -device virtio-rng-pci \
    -device virtio-net-pci,netdev=net0 \
    -audiodev ${BROOK_AUDIODEV:-pipewire},id=hda0,out.buffer-length=200000,timer-period=5000 \
    -device ich9-intel-hda,bus=pcie.0,addr=0x1b \
    -device hda-output,audiodev=hda0 \
    -netdev user,id=net0,hostfwd=tcp::11237-:1234 \
    ${SERIAL_OPT} \
    ${DISPLAY_OPT} \
    -monitor unix:/tmp/qemu_monitor.sock,server,nowait \
    -no-reboot \
    -no-shutdown \
    ${DEBUG_FLAGS} \
    "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"

# If a profile was written to the boot disk, offer to extract it.
if mcopy -i "${DISK_IMG}" ::profile.txt /dev/null 2>/dev/null; then
    echo ""
    echo "  Profile found on boot disk. To extract and convert:"
    echo "    mcopy -i ${DISK_IMG} ::profile.txt ./profile.txt"
    echo "    python3 ${SCRIPT_DIR}/profiler_to_speedscope.py profile.txt"
fi
