#!/usr/bin/env bash
# prestage_yquake2.sh — install Yamagi Quake II (yquake2) into the Brook nix disk
# image OFFLINE, so it runs without a live nix-install fetch.
#
# WHY: nix-install of an UNcached package currently fails because Brook's guest
# DNS can't resolve cache.nixos.org (BRO-200, a recent regression). Until that's
# fixed, this stages yquake2's closure directly into brook_nix_disk.img from the
# HOST's nix store (host DNS/networking is fine) using the same fuse2fs +
# store-copy + profile-symlink mechanism as update_nix_disk.sh.
#
# Idempotent: only missing store paths are copied; re-running is a near no-op.
# Requires nix-shell (fuse2fs, nix-build/nix-store) and a host that can realise
# nixpkgs#yquake2 (will substitute from cache.nixos.org if not already present).
#
# Usage: nix-shell --run ./scripts/prestage_yquake2.sh
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

echo "Realising yquake2 closure on the host (substitutes from cache if needed)..."
YQ2_OUT="$(nix-build '<nixpkgs>' -A yquake2 --no-out-link 2>/dev/null \
            || nix build --no-link --print-out-paths nixpkgs#yquake2 | tail -1)"
if [ -z "${YQ2_OUT}" ] || [ ! -x "${YQ2_OUT}/bin/yquake2" ]; then
    echo "ERROR: failed to realise nixpkgs#yquake2 on the host." >&2
    exit 1
fi
YQ2_BASE="$(basename "${YQ2_OUT}")"
echo "  yquake2 -> ${YQ2_OUT}"

MNTDIR="$(mktemp -d)"
cleanup() {
    fusermount -u "${MNTDIR}" 2>/dev/null || fusermount -uz "${MNTDIR}" 2>/dev/null || true
    rmdir "${MNTDIR}" 2>/dev/null || true
}
trap cleanup EXIT

fuse2fs -o rw,fakeroot "${DISK_IMG}" "${MNTDIR}"
echo "Mounted ${DISK_IMG} at ${MNTDIR}"

mkdir -p "${MNTDIR}/store" "${MNTDIR}/profile/bin"

# Copy every closure path that isn't already on the disk (most are shared with
# the existing STK/gltron installs, so this is a small delta).
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
done < <(nix-store -qR "${YQ2_OUT}")
echo "Copied ${ADDED} new store path(s)."

# Expose the binaries via /nix/profile/bin using the SAME relative-symlink form
# nix-install uses (../../store/<pkg>/bin/<name>) so Brook's VFS resolves them.
for binname in yquake2 yq2ded; do
    if [ -e "${MNTDIR}/store/${YQ2_BASE}/bin/${binname}" ]; then
        ln -sfn "../../store/${YQ2_BASE}/bin/${binname}" "${MNTDIR}/profile/bin/${binname}"
        echo "  profile/bin/${binname} -> ../../store/${YQ2_BASE}/bin/${binname}"
    fi
done

# Generate /nix/yquake2-play.sh from the template, baking in the ABSOLUTE store
# path of the binary we just staged. This mirrors how nix-install generates
# stk-play.sh: the wrapper execs an absolute path directly. Critically it avoids
# any $(command -v ...) command substitution, which on Brook forks a bash
# subshell that re-execs bash and fails ("cannot execute binary file", exit 126).
TEMPLATE="${ROOT_DIR}/data/nix-wrappers/yquake2-play.sh.in"
if [ ! -f "${TEMPLATE}" ]; then
    echo "ERROR: wrapper template not found: ${TEMPLATE}" >&2
    exit 1
fi
WRAPPER_TMP="$(mktemp)"
sed "s#@YQUAKE2_BIN@#/nix/store/${YQ2_BASE}/bin/yquake2#g" "${TEMPLATE}" > "${WRAPPER_TMP}"
cp "${WRAPPER_TMP}" "${MNTDIR}/yquake2-play.sh"
chmod 0755 "${MNTDIR}/yquake2-play.sh"
rm -f "${WRAPPER_TMP}"
echo "  /nix/yquake2-play.sh -> exec /nix/store/${YQ2_BASE}/bin/yquake2"

sync
echo "Done. yquake2 staged into ${DISK_IMG}."
echo "Run it with the GPU compositor:"
echo "  BROOK_GPU=gl BROOK_GPU_DISPLAY=sdl BROOK_COMPOSITE=gpu ./scripts/run-qemu.sh --release --script wayland_yquake2_gl"
