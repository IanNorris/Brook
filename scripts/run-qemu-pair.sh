#!/usr/bin/env bash
# Launches two Brook VMs on a shared VDE switch so they can see each other
# (and reach the host/internet via slirpvde's NAT gateway at 10.42.0.2).
#
# Instance 0 uses the primary disk images; instance 1 gets per-instance
# copies under /tmp/brook-vm1/ so the two VMs don't corrupt each other's
# filesystems.
#
# Usage: scripts/run-qemu-pair.sh [--release] [extra args forwarded to both]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

BUILD_TYPE_ARG=""
EXTRA=()
for arg in "$@"; do
    case "$arg" in
        --release) BUILD_TYPE_ARG="--release" ;;
        --debug)   ;; # intentionally ignored — only one instance can own :1234
        *)         EXTRA+=("$arg") ;;
    esac
done

# 1) Make sure the VDE switch is up
"${SCRIPT_DIR}/vde-up.sh" up

VDE_SOCK=/tmp/vde-brook.ctl

# 2) Prepare per-VM disk images for instance 1 (copy on first use).
VM1_DIR=/tmp/brook-vm1
mkdir -p "${VM1_DIR}"

clone_if_exists() {
    local src="$1"
    local dst="$2"
    if [ -f "${src}" ] && [ ! -f "${dst}" ]; then
        echo "  cloning $(basename "${src}") -> ${dst}"
        cp --reflink=auto "${src}" "${dst}"
    fi
}

echo "Preparing VM1 disk images in ${VM1_DIR}..."
clone_if_exists "${ROOT_DIR}/brook_disk.img"       "${VM1_DIR}/brook_disk.img"
clone_if_exists "${ROOT_DIR}/brook_ext2_disk.img"  "${VM1_DIR}/brook_ext2_disk.img"
clone_if_exists "${ROOT_DIR}/brook_nix_disk.img"   "${VM1_DIR}/brook_nix_disk.img"
clone_if_exists "${ROOT_DIR}/brook_home_disk.img"  "${VM1_DIR}/brook_home_disk.img"

# Per-instance ESP directories.  We build them from scratch (bootloader +
# kernel + BROOK.CFG + initrd) rather than from build/{type}/esp so the
# pair script works even on a fresh checkout where run-qemu.sh hasn't
# populated that directory yet.
BUILD_TYPE="${BUILD_TYPE_ARG#--}"
BUILD_TYPE="${BUILD_TYPE:-debug}"
BUILD_DIR="${ROOT_DIR}/build/${BUILD_TYPE}"
BOOTLOADER="${BUILD_DIR}/bootloader/BOOTX64.efi"
KERNEL_ELF="${BUILD_DIR}/kernel/BROOK.elf"

if [ ! -f "${BOOTLOADER}" ] || [ ! -f "${KERNEL_ELF}" ]; then
    echo "ERROR: bootloader or kernel missing in ${BUILD_DIR}."
    echo "Build first:  scripts/build.sh ${BUILD_TYPE^}"
    exit 1
fi

# Make sure initrd exists (run-qemu.sh normally regenerates it).
"${SCRIPT_DIR}/make_initrd.sh" "--${BUILD_TYPE}" >/dev/null

VM0_DIR=/tmp/brook-vm0
VM0_ESP="${VM0_DIR}/esp"
VM1_ESP="${VM1_DIR}/esp"
mkdir -p "${VM0_DIR}"
rm -rf "${VM0_ESP}" "${VM1_ESP}"

build_esp() {
    local dst="$1"
    local ip="$2"
    mkdir -p "${dst}/EFI/BOOT" "${dst}/KERNEL"
    cp "${BOOTLOADER}" "${dst}/EFI/BOOT/BOOTX64.EFI"
    cp "${KERNEL_ELF}" "${dst}/KERNEL/BROOK.ELF"
    # Initrd (contains early drivers + any config)
    if [ -f "${BUILD_DIR}/initrd.img" ]; then
        cp "${BUILD_DIR}/initrd.img" "${dst}/INITRD.IMG"
    fi
    cat > "${dst}/BROOK.CFG" <<EOF
# Brook OS boot configuration (pair instance)
TARGET=KERNEL\\BROOK.ELF
DEBUG_TEXT=0
LOG_MEMORY=0
LOG_INTERRUPTS=0
NET0_MODE=static
NET0_IP=${ip}
NET0_NETMASK=255.255.255.0
EOF
}

build_esp "${VM0_ESP}" 10.42.0.10
build_esp "${VM1_ESP}" 10.42.0.11
echo "VM0 static IP: 10.42.0.10   VM1 static IP: 10.42.0.11"

# 3) Launch both VMs. Instance 0 in the background, instance 1 in foreground
#    (so Ctrl-C from the terminal reaches the second window cleanly).
echo ""
echo "Launching VM0 (MAC 52:54:00:42:00:10, IP 10.42.0.10)..."
echo "  (output → /tmp/brook-vm0.log)"
ESP_OVERRIDE="${VM0_ESP}" \
"${SCRIPT_DIR}/run-qemu.sh" ${BUILD_TYPE_ARG} \
    --vde="${VDE_SOCK}" \
    --mac=52:54:00:42:00:10 \
    --instance=0 \
    --no-audio \
    "${EXTRA[@]+"${EXTRA[@]}"}" </dev/null >/tmp/brook-vm0.log 2>&1 &
VM0_PID=$!

sleep 3

if ! kill -0 "${VM0_PID}" 2>/dev/null; then
    echo "ERROR: VM0 exited early. Last 30 lines of /tmp/brook-vm0.log:"
    tail -30 /tmp/brook-vm0.log
    exit 1
fi

echo "Launching VM1 (MAC 52:54:00:42:00:11, IP 10.42.0.11)..."
BROOK_DISK_IMG="${VM1_DIR}/brook_disk.img" \
BROOK_EXT2_DISK="${VM1_DIR}/brook_ext2_disk.img" \
BROOK_NIX_DISK="${VM1_DIR}/brook_nix_disk.img" \
BROOK_HOME_DISK="${VM1_DIR}/brook_home_disk.img" \
ESP_OVERRIDE="${VM1_ESP}" \
"${SCRIPT_DIR}/run-qemu.sh" ${BUILD_TYPE_ARG} \
    --vde="${VDE_SOCK}" \
    --mac=52:54:00:42:00:11 \
    --instance=1 \
    --no-audio \
    "${EXTRA[@]+"${EXTRA[@]}"}"

# VM1 exited; clean up VM0 if still alive.
if kill -0 "${VM0_PID}" 2>/dev/null; then
    echo "VM1 exited; stopping VM0..."
    kill "${VM0_PID}" || true
    wait "${VM0_PID}" 2>/dev/null || true
fi

echo ""
echo "Both VMs exited. To tear down the VDE switch: scripts/vde-up.sh down"
