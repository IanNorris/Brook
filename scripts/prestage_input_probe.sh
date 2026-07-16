#!/usr/bin/env bash
# prestage_input_probe.sh — install the BRO-216 input probes into the Brook nix
# disk image OFFLINE, so they run without a live nix-install fetch.
#
# Stages the tools/sdl2-input-probe closure (wl_keymap_probe, xkb_memfd_probe,
# kbdprobe) into brook_nix_disk.img using the same fuse2fs + store-copy +
# profile-symlink mechanism as prestage_yquake2.sh. Idempotent: only missing
# store paths are copied.
#
# The decisive probe, wl_keymap_probe, is a raw-libwayland client that receives
# the REAL keymap event from waylandd via SCM_RIGHTS and runs SDL3's exact
# mmap(MAP_PRIVATE)+xkb compile+text path, pinpointing where BRO-216 fails.
#
# Usage: nix-shell --run ./scripts/prestage_input_probe.sh
#   then boot any Wayland config (so waylandd is up) and run, from a terminal:
#     wl_keymap_probe
#   or boot headless with the bundled rc and read the result on serial:
#     ./scripts/run-qemu.sh --release --headless --script wayland_keymap_probe
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
DISK_IMG="${BROOK_NIX_DISK:-${ROOT_DIR}/brook_nix_disk.img}"

if [ ! -f "${DISK_IMG}" ]; then
    echo "ERROR: nix disk not found: ${DISK_IMG}" >&2
    exit 1
fi
for tool in fuse2fs nix-build nix-store; do
    command -v "$tool" &>/dev/null || { echo "ERROR: $tool not found. Run inside nix-shell." >&2; exit 1; }
done

echo "Building the input probes on the host..."
PROBE_OUT="$(nix-build "${ROOT_DIR}/tools/sdl2-input-probe" --no-out-link 2>/dev/null)"
if [ -z "${PROBE_OUT}" ] || [ ! -x "${PROBE_OUT}/bin/wl_keymap_probe" ]; then
    echo "ERROR: failed to build tools/sdl2-input-probe." >&2
    exit 1
fi
PROBE_BASE="$(basename "${PROBE_OUT}")"
echo "  input-probe -> ${PROBE_OUT}"

MNTDIR="$(mktemp -d)"
cleanup() {
    fusermount -u "${MNTDIR}" 2>/dev/null || fusermount -uz "${MNTDIR}" 2>/dev/null || true
    rmdir "${MNTDIR}" 2>/dev/null || true
}
trap cleanup EXIT

fuse2fs -o rw,fakeroot "${DISK_IMG}" "${MNTDIR}"
echo "Mounted ${DISK_IMG} at ${MNTDIR}"

mkdir -p "${MNTDIR}/store" "${MNTDIR}/profile/bin"

ADDED=0
while IFS= read -r p; do
    [ -n "$p" ] || continue
    base="$(basename "$p")"
    dst="${MNTDIR}/store/${base}"
    if [ ! -e "$dst" ]; then
        cp -a --no-preserve=links "$p" "$dst"
        ADDED=$((ADDED + 1))
        echo "  + store/${base}"
    fi
done < <(nix-store -qR "${PROBE_OUT}")
echo "Copied ${ADDED} new store path(s)."

# Expose the probe binaries via /nix/profile/bin using the same relative-symlink
# form nix-install uses, so Brook's VFS resolves them from $PATH.
for binname in wl_keymap_probe xkb_memfd_probe kbdprobe; do
    if [ -e "${MNTDIR}/store/${PROBE_BASE}/bin/${binname}" ]; then
        ln -sfn "../../store/${PROBE_BASE}/bin/${binname}" "${MNTDIR}/profile/bin/${binname}"
        echo "  profile/bin/${binname} -> ../../store/${PROBE_BASE}/bin/${binname}"
    fi
done

sync
echo "Done. Input probes staged into ${DISK_IMG}."
echo
echo "Run the decisive probe (from a terminal while waylandd is up):"
echo "  wl_keymap_probe"
echo "Or boot headless and read the result on serial:"
echo "  ./scripts/run-qemu.sh --release --headless --script wayland_keymap_probe"
