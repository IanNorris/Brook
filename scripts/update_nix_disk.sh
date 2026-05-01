#!/usr/bin/env bash
# Update tools and index on an existing Nix store disk image.
#
# Usage:
#   scripts/update_nix_disk.sh [--tools] [--index] [--all]
#
# Without flags, updates both tools and index (same as --all).
# Uses fuse2fs to mount, update in place, and unmount.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
DISK_IMG="${BROOK_NIX_DISK:-${ROOT_DIR}/brook_nix_disk.img}"
TOOLS_BIN="${ROOT_DIR}/tools"
INDEX_FILE="${ROOT_DIR}/tools/nix-index/packages.idx"

UPDATE_TOOLS=0
UPDATE_INDEX=0

if [ $# -eq 0 ]; then
    UPDATE_TOOLS=1
    UPDATE_INDEX=1
fi

for arg in "$@"; do
    case "$arg" in
        --tools) UPDATE_TOOLS=1 ;;
        --index) UPDATE_INDEX=1 ;;
        --all)   UPDATE_TOOLS=1; UPDATE_INDEX=1 ;;
        *)       echo "Unknown arg: $arg"; echo "Usage: $0 [--tools] [--index] [--all]"; exit 1 ;;
    esac
done

if [ ! -f "${DISK_IMG}" ]; then
    echo "Nix disk not found: ${DISK_IMG}"
    echo "Run: scripts/create_nix_disk.sh"
    exit 1
fi

if ! command -v fuse2fs &>/dev/null; then
    echo "ERROR: fuse2fs not found. Run inside nix-shell."
    exit 1
fi

MNTDIR=$(mktemp -d)
cleanup() {
    sync
    fusermount -u "${MNTDIR}" 2>/dev/null || fusermount -uz "${MNTDIR}" 2>/dev/null || true
    rmdir "${MNTDIR}" 2>/dev/null || true
}
trap cleanup EXIT

fuse2fs -o rw,fakeroot "${DISK_IMG}" "${MNTDIR}"
echo "Mounted ${DISK_IMG} at ${MNTDIR}"

if [ "${UPDATE_TOOLS}" -eq 1 ]; then
    echo "Updating tools..."
    mkdir -p "${MNTDIR}/bin"
    for tool in nix-fetch nix-search nix-install; do
        src="${TOOLS_BIN}/${tool}/${tool}"
        if [ -f "$src" ]; then
            cp "$src" "${MNTDIR}/bin/${tool}"
            echo "  ${tool} -> /nix/bin/${tool}"
        else
            echo "  WARNING: ${src} not found"
        fi
    done
    nar="${TOOLS_BIN}/nar-unpack/nar-unpack"
    if [ -f "$nar" ]; then
        cp "$nar" "${MNTDIR}/bin/nar-unpack"
        echo "  nar-unpack -> /nix/bin/nar-unpack"
    fi
    # Ensure nix -> nix-install symlink exists
    ln -sf nix-install "${MNTDIR}/bin/nix"

    # Bundle curl, xz binaries and CA certs from the on-disk store so
    # nix-fetch finds them at stable paths (/nix/bin/curl, /nix/bin/xz,
    # /nix/etc/ssl/certs/ca-bundle.crt). Without these, `nix install`
    # fails immediately with "curl not found".
    if [ -d "${MNTDIR}/store" ]; then
        for d in "${MNTDIR}"/store/*-curl-*-bin; do
            [ -x "$d/bin/curl" ] || continue
            cp "$d/bin/curl" "${MNTDIR}/bin/curl"
            echo "  curl -> /nix/bin/curl"
            break
        done
        for d in "${MNTDIR}"/store/*-xz-*-bin; do
            [ -x "$d/bin/xz" ] || continue
            cp "$d/bin/xz" "${MNTDIR}/bin/xz"
            echo "  xz -> /nix/bin/xz"
            break
        done
        for d in "${MNTDIR}"/store/*-waylandd-brook-*; do
            [ -x "$d/bin/waylandd" ] || continue
            cp "$d/bin/waylandd" "${MNTDIR}/bin/waylandd"
            echo "  waylandd -> /nix/bin/waylandd"
            break
        done
        for d in "${MNTDIR}"/store/*-nss-cacert-* "${MNTDIR}"/store/*-cacert-*; do
            [ -f "$d/etc/ssl/certs/ca-bundle.crt" ] || continue
            mkdir -p "${MNTDIR}/etc/ssl/certs"
            cp "$d/etc/ssl/certs/ca-bundle.crt" \
                "${MNTDIR}/etc/ssl/certs/ca-bundle.crt"
            echo "  ca-bundle.crt -> /nix/etc/ssl/certs/ca-bundle.crt"
            break
        done
    fi
fi

if [ "${UPDATE_INDEX}" -eq 1 ]; then
    if [ ! -f "${INDEX_FILE}" ]; then
        echo "Index not found: ${INDEX_FILE}"
        echo "Run: python3 tools/nix-index/gen-nix-index.py --output ${INDEX_FILE}"
        exit 1
    fi
    mkdir -p "${MNTDIR}/index"
    cp "${INDEX_FILE}" "${MNTDIR}/index/packages.idx"
    echo "Updated index: $(wc -l < "${INDEX_FILE}") packages"
fi

sync
echo "Done."
