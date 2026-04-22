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

# Per-instance ESP directories (copies of build/{type}/esp) so the two
# QEMUs don't fight over fat:rw: on the same directory, and so we can
# stamp per-instance BROOK.CFG with distinct static IPs.
BUILD_TYPE="${BUILD_TYPE_ARG#--}"
BUILD_TYPE="${BUILD_TYPE:-debug}"
SRC_ESP="${ROOT_DIR}/build/${BUILD_TYPE}/esp"
VM0_DIR=/tmp/brook-vm0
VM0_ESP="${VM0_DIR}/esp"
VM1_ESP="${VM1_DIR}/esp"
mkdir -p "${VM0_DIR}"
rm -rf "${VM0_ESP}" "${VM1_ESP}"
cp -r "${SRC_ESP}" "${VM0_ESP}"
cp -r "${SRC_ESP}" "${VM1_ESP}"

# Strip any existing NET0_* lines, then append per-instance static config.
stamp_brook_cfg() {
    local cfg="$1"
    local ip="$2"
    if [ -f "${cfg}" ]; then
        grep -v '^NET0_' "${cfg}" > "${cfg}.tmp" || true
        mv "${cfg}.tmp" "${cfg}"
    else
        touch "${cfg}"
    fi
    cat >> "${cfg}" <<EOF
NET0_MODE=static
NET0_IP=${ip}
NET0_NETMASK=255.255.255.0
EOF
}
stamp_brook_cfg "${VM0_ESP}/BROOK.CFG" 10.42.0.10
stamp_brook_cfg "${VM1_ESP}/BROOK.CFG" 10.42.0.11
echo "VM0 static IP: 10.42.0.10   VM1 static IP: 10.42.0.11"

# 3) Launch both VMs. Instance 0 in the background, instance 1 in foreground
#    (so Ctrl-C from the terminal reaches the second window cleanly).
echo ""
echo "Launching VM0 (MAC 52:54:00:42:00:10, IP 10.42.0.10)..."
ESP_OVERRIDE="${VM0_ESP}" \
"${SCRIPT_DIR}/run-qemu.sh" ${BUILD_TYPE_ARG} \
    --vde="${VDE_SOCK}" \
    --mac=52:54:00:42:00:10 \
    --instance=0 \
    --no-audio \
    "${EXTRA[@]+"${EXTRA[@]}"}" &
VM0_PID=$!

sleep 2

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
