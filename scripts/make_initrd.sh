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
# BROOK_CA_BUNDLE selects the root CA bundle (set by shell.nix).
# BROOK_PROVIDER_PAYLOAD optionally packages a validated SteamRT provider cache
# and its matching installed host loader (steamrt_provider_cache.py --loader).
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

# Override for an extracted guest build or another isolated artifact directory.
BUILD_DIR="${BROOK_BUILD_DIR:-${ROOT_DIR}/build/${BUILD_TYPE}}"
ESP_DIR="${BUILD_DIR}/esp"
INITRD_IMG="${ESP_DIR}/INITRD.IMG"
MOD_DIR="${BUILD_DIR}/kernel/drivers"

# NSS/p11-kit reads standard system trust paths rather than SSL_CERT_FILE.
# Keep certificate verification usable for stock applications from a fresh boot.
CA_BUNDLE="${BROOK_CA_BUNDLE:-${SSL_CERT_FILE:-/etc/ssl/certs/ca-certificates.crt}}"
if [ ! -s "${CA_BUNDLE}" ] || [ ! -r "${CA_BUNDLE}" ]; then
    echo "Missing CA bundle: ${CA_BUNDLE}; run inside nix-shell or set BROOK_CA_BUNDLE." >&2
    exit 1
fi

# Image size in KB — enough for config + all driver modules.
# 2880KB (2.88MB floppy) accommodates the ext4 module (~800KB debug) plus others.
SIZE_KB=2880

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

# Create /tmp — needed by graphical apps for runtime state (Wayland sockets,
# LibreOffice user profile, XDG_RUNTIME_DIR).
mmd -i "${INITRD_IMG}.tmp" "::TMP" 2>/dev/null || true

# The kernel implements named shared-memory opens under /dev/shm. Provide the
# directory so applications discover that backend on a fresh boot.
mmd -i "${INITRD_IMG}.tmp" "::DEV" "::DEV/SHM"

# Keep /usr a real retained directory even with Nix-managed userland. Container
# tools bind this host-layout entry; a synthetic executable alias is insufficient.
mmd -i "${INITRD_IMG}.tmp" "::USR"

# Minimal /sys stub — some apps read /sys/devices/system/cpu/online.
mmd -i "${INITRD_IMG}.tmp" "::SYS" 2>/dev/null || true
mmd -i "${INITRD_IMG}.tmp" "::SYS/DEVICES" 2>/dev/null || true
mmd -i "${INITRD_IMG}.tmp" "::SYS/DEVICES/SYSTEM" 2>/dev/null || true
mmd -i "${INITRD_IMG}.tmp" "::SYS/DEVICES/SYSTEM/CPU" 2>/dev/null || true
echo "0-7" | mcopy -i "${INITRD_IMG}.tmp" - "::SYS/DEVICES/SYSTEM/CPU/ONLINE"

# GLib/GIO and DBus require a valid machine-id even for session-bus
# autolaunch. A deterministic ID is sufficient for Brook's single-VM
# development images and avoids toolkit startup stalls.
mmd -i "${INITRD_IMG}.tmp" "::ETC" 2>/dev/null || true
echo "0123456789abcdef0123456789abcdef" | \
    mcopy -i "${INITRD_IMG}.tmp" - "::ETC/MACHINE-ID"

mmd -i "${INITRD_IMG}.tmp" "::ETC/SSL" "::ETC/SSL/CERTS"
mcopy -i "${INITRD_IMG}.tmp" "${CA_BUNDLE}" "::ETC/SSL/CERTS/ca-certificates.crt"
echo "  packed: /etc/ssl/certs/ca-certificates.crt"

if [ -n "${BROOK_PROVIDER_PAYLOAD:-}" ]; then
    python3 - "${BROOK_PROVIDER_PAYLOAD}" <<'PY'
from pathlib import Path
import hashlib, json, sys
p = Path(sys.argv[1])
m = json.loads((p / 'manifest.json').read_text())
for name, expected in [('ld.so.cache', m['cache_sha256']),
                       ('ld.so.conf', m['config_sha256']),
                       ('ld-linux-x86-64.so.2', m['loader']['sha256'])]:
    if hashlib.sha256((p / name).read_bytes()).hexdigest() != expected:
        raise SystemExit('Provider payload hash mismatch: ' + name)
data = (p / 'ld-linux-x86-64.so.2').read_bytes()
if data[:6] != b'\x7fELF\x02\x01' or data[18:20] != b'\x3e\x00':
    raise SystemExit('Provider loader must be x86-64 ELF')
PY
    mmd -i "${INITRD_IMG}.tmp" "::lib64"
    # mcopy must succeed before atomic publication. Insufficient initrd space
    # fails here and the trap removes the unpublished temporary image.
    mcopy -i "${INITRD_IMG}.tmp" "${BROOK_PROVIDER_PAYLOAD}/ld.so.cache" "::ETC/ld.so.cache"
    mcopy -i "${INITRD_IMG}.tmp" "${BROOK_PROVIDER_PAYLOAD}/ld.so.conf" "::ETC/ld.so.conf"
    mcopy -i "${INITRD_IMG}.tmp" "${BROOK_PROVIDER_PAYLOAD}/ld-linux-x86-64.so.2" "::lib64/ld-linux-x86-64.so.2"
    python3 - "${BROOK_PROVIDER_PAYLOAD}" "${INITRD_IMG}.tmp" <<'PY'
from pathlib import Path
import hashlib, json, subprocess, sys
m = json.loads((Path(sys.argv[1]) / 'manifest.json').read_text())
for target, expected in [('::ETC/ld.so.cache', m['cache_sha256']),
                         ('::ETC/ld.so.conf', m['config_sha256']),
                         ('::lib64/ld-linux-x86-64.so.2', m['loader']['sha256'])]:
    data = subprocess.run(['mtype', '-i', sys.argv[2], target],
                          capture_output=True, check=True).stdout
    if hashlib.sha256(data).hexdigest() != expected:
        raise SystemExit('Provider initrd read-back mismatch: ' + target)
PY
    echo "  packed: validated provider cache and /lib64/ld-linux-x86-64.so.2"
fi

# Move into place
mv "${INITRD_IMG}.tmp" "${INITRD_IMG}"

echo "Initrd image created: ${INITRD_IMG} ($(stat -c%s "${INITRD_IMG}") bytes)"
echo ""
mdir -i "${INITRD_IMG}" :: 2>/dev/null || true
