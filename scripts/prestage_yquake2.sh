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

echo "Building the Brook yquake2 (stock + brook-features.patch: durable settings"
echo "+ frametime overlay) on the host..."
YQ2_OUT="$(nix-build "${ROOT_DIR}/tools/yquake2-brook-pkg" --no-out-link 2>/dev/null)"
if [ -z "${YQ2_OUT}" ] || [ ! -x "${YQ2_OUT}/bin/yquake2" ]; then
    echo "ERROR: failed to build tools/yquake2-brook-pkg." >&2
    exit 1
fi
YQ2_BASE="$(basename "${YQ2_OUT}")"
echo "  yquake2(brook) -> ${YQ2_OUT}"

# Realise the Brook OSS build of openal-soft. yquake2 dlopen()s libopenal.so.1
# by bare soname; the stock closure openal has only pulse/alsa/wave backends,
# none of which exist on Brook. This build adds the OSS backend that drives
# /dev/dsp. Its lib dir is prepended to the wrapper's LD_LIBRARY_PATH below so
# it wins the dlopen over the stock closure openal.
echo "Realising openal-soft (OSS backend) on the host..."
OPENAL_OSS_OUT="$(nix-build "${ROOT_DIR}/tools/openal-soft-oss-pkg" --no-out-link 2>/dev/null)"
if [ -z "${OPENAL_OSS_OUT}" ] || [ ! -e "${OPENAL_OSS_OUT}/lib/libopenal.so.1" ]; then
    echo "ERROR: failed to build tools/openal-soft-oss-pkg (OSS openal)." >&2
    exit 1
fi
OPENAL_OSS_BASE="$(basename "${OPENAL_OSS_OUT}")"
echo "  openal-soft(OSS) -> ${OPENAL_OSS_OUT}"

MNTDIR="$(mktemp -d)"
# EXT2_MNT is set later when the /data image is mounted. Track it here so the
# EXIT trap ALWAYS unmounts it — otherwise a failure between mount and the inline
# unmount (e.g. a chown error under set -e) leaves a dangling fuse2fs mount, and
# the next run's e2fsck refuses with "filesystem is mounted".
EXT2_MNT=""
cleanup() {
    fusermount -u "${MNTDIR}" 2>/dev/null || fusermount -uz "${MNTDIR}" 2>/dev/null || true
    rmdir "${MNTDIR}" 2>/dev/null || true
    if [ -n "${EXT2_MNT}" ]; then
        fusermount -u "${EXT2_MNT}" 2>/dev/null || fusermount -uz "${EXT2_MNT}" 2>/dev/null || true
        rmdir "${EXT2_MNT}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

fuse2fs -o rw,fakeroot "${DISK_IMG}" "${MNTDIR}"
echo "Mounted ${DISK_IMG} at ${MNTDIR}"

mkdir -p "${MNTDIR}/store" "${MNTDIR}/profile/bin"

# Copy every closure path that isn't already on the disk (most are shared with
# the existing STK/gltron installs, so this is a small delta). Stage both the
# yquake2 closure AND the OSS openal closure (the latter pulls in only a few
# extra paths beyond what yquake2 already shares).
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
done < <(nix-store -qR "${YQ2_OUT}" "${OPENAL_OSS_OUT}")
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

# Resolve the REAL binary path (lib/yquake2/quake2), not the bin/yquake2 symlink:
# yquake2 locates its renderer .so's relative to /proc/self/exe, which Brook
# reports as the exec'd path. Exec'ing the symlink would make it look for
# renderers in bin/ (they live in lib/yquake2/). Map the host store path back to
# the guest /nix/store path for the wrapper.
YQ2_REAL_HOST="$(readlink -f "${YQ2_OUT}/bin/yquake2")"          # /nix/store/<base>/lib/yquake2/quake2 (host)
YQ2_REAL_GUEST="/nix/store/${YQ2_BASE}/${YQ2_REAL_HOST#"${YQ2_OUT}/"}"
[ "${YQ2_REAL_GUEST}" = "/nix/store/${YQ2_BASE}/" ] && YQ2_REAL_GUEST="/nix/store/${YQ2_BASE}/lib/yquake2/quake2"

# Lib dirs for bare-soname dlopen()s not on the binary's rpath. The Brook OSS
# openal-soft MUST come first so yquake2's dlopen("libopenal.so.1") resolves to
# it rather than the stock closure openal (pulse/alsa/wave only). Then the
# renderer dir as a fallback. LD_LIBRARY_PATH is searched before DT_RUNPATH, so
# prepending the OSS openal here reliably wins the resolution.
LIBDIRS="/nix/store/${OPENAL_OSS_BASE}/lib:/nix/store/${YQ2_BASE}/lib/yquake2"

WRAPPER_TMP="$(mktemp)"
sed -e "s#@YQUAKE2_BIN@#${YQ2_REAL_GUEST}#g" \
    -e "s#@YQUAKE2_LIBDIRS@#${LIBDIRS}#g" \
    "${TEMPLATE}" > "${WRAPPER_TMP}"
cp "${WRAPPER_TMP}" "${MNTDIR}/yquake2-play.sh"
chmod 0755 "${MNTDIR}/yquake2-play.sh"
rm -f "${WRAPPER_TMP}"
echo "  /nix/yquake2-play.sh -> exec ${YQ2_REAL_GUEST}"
echo "  LD_LIBRARY_PATH = ${LIBDIRS}"

sync

# --- Create a uid-1000-owned HOME on the ext2 /data disk -----------------------
# yquake2 runs as uid 1000 (it refuses to run as root), and the ext2 /data mount
# enforces DAC, so HOME=/data/yq2 must already exist owned by uid 1000 — else
# yquake2 dies with "Couldn't create dir /data/yq2/.yq2/". Precedent: /data/tmp
# is uid 1000:100 too. fuse2fs fakeroot lets us chown without privilege.
EXT2_IMG="${BROOK_EXT2_DISK:-${ROOT_DIR}/brook_ext2_disk.img}"
DATA_OK=0
if [ -f "${EXT2_IMG}" ]; then
    # Clean up any stale fuse2fs mount of this image left by a previously failed
    # run (e.g. one that aborted between mount and unmount). e2fsck refuses to
    # touch a mounted filesystem ("... is mounted"), so we must unmount first.
    EXT2_IMG_REAL="$(readlink -f "${EXT2_IMG}")"
    while IFS= read -r stale_mp; do
        [ -n "${stale_mp}" ] || continue
        echo "  unmounting stale fuse2fs mount at ${stale_mp}"
        fusermount -u "${stale_mp}" 2>/dev/null || fusermount -uz "${stale_mp}" 2>/dev/null || true
    done < <(awk -v img="${EXT2_IMG_REAL}" '$1==img {print $2}' /proc/mounts 2>/dev/null)

    # Repair the ext2 image before mounting. yquake2 (and other guests) write to
    # /data, and if a prior guest run left the filesystem inconsistent, fuse2fs
    # hits EUCLEAN ("Structure needs cleaning") mid-operation and flips the mount
    # read-only — so the chown below fails and HOME never becomes uid-1000-owned.
    # e2fsck -fy auto-repairs (orphaned inodes, bad counts) so the mount is clean.
    # This is safe: the image is not mounted here yet (stale mounts cleaned
    # above), and prestage runs on the host against the on-disk image, not a
    # live-booted disk. Do NOT run this while QEMU is booted against the image.
    if command -v e2fsck >/dev/null 2>&1; then
        echo "Checking ${EXT2_IMG} (e2fsck -fy)..."
        e2fsck -fy "${EXT2_IMG}" 2>&1 | sed 's/^/  /' || \
            echo "  (e2fsck reported issues; it repairs what it can — continuing)"
    fi
    EXT2_MNT="$(mktemp -d)"
    if fuse2fs -o rw,fakeroot "${EXT2_IMG}" "${EXT2_MNT}" 2>/dev/null; then
        if mkdir -p "${EXT2_MNT}/yq2/.yq2/baseq2" 2>/dev/null && \
           chown -R 1000:100 "${EXT2_MNT}/yq2" 2>/dev/null; then
            echo "  /data/yq2 (HOME) + .yq2/baseq2 created on ${EXT2_IMG}, owned uid 1000:100"
        else
            echo "  ERROR: failed to create/chown /data/yq2 — the ext2 image may still be" >&2
            echo "         corrupt (fuse2fs flipped read-only). Run: e2fsck -fy ${EXT2_IMG}" >&2
            echo "         then re-run this script." >&2
        fi
        # yquake2 reads the game data from the SAME path the native software
        # Quake II port uses — /data/games/quake2/baseq2/pak0.pak — via -datadir
        # in the wrapper. So the two SHARE one copy; nothing to duplicate or
        # symlink. Just verify it's present (it's staged by update_ext2_disk.sh);
        # if it's missing, point the user there rather than copying it here.
        Q2_PAK_DST="${EXT2_MNT}/games/quake2/baseq2/pak0.pak"
        if [ -f "${Q2_PAK_DST}" ]; then
            echo "  game data present (shared with native Quake II): /data/games/quake2/baseq2/pak0.pak ($(du -h "${Q2_PAK_DST}" | cut -f1))"
            DATA_OK=1
        else
            echo "  WARNING: /data/games/quake2/baseq2/pak0.pak is MISSING." >&2
            echo "           yquake2 shares the native Quake II data via -datadir; stage it with:" >&2
            echo "             ./scripts/update_ext2_disk.sh           # (BROOK_Q2_PAK=/path/to/pak0.pak to override)" >&2
            echo "           If the native software Quake II loads on this disk, yquake2 will too." >&2
        fi

        sync
        fusermount -u "${EXT2_MNT}" 2>/dev/null || fusermount -uz "${EXT2_MNT}" 2>/dev/null || true
    else
        echo "  WARNING: could not mount ${EXT2_IMG}; create /data/yq2 (uid 1000) manually." >&2
    fi
    rmdir "${EXT2_MNT}" 2>/dev/null || true
    EXT2_MNT=""   # unmounted above — clear so the EXIT trap doesn't retry
else
    echo "  NOTE: ext2 /data disk not found (${EXT2_IMG}); create /data/yq2 owned by uid 1000 yourself." >&2
fi

echo "Done. yquake2 staged into ${DISK_IMG}."
[ "${DATA_OK}" = "1" ] || echo "  (!) Game data not confirmed — see the warning above before launching."
echo "Run it with the GPU compositor:"
echo "  BROOK_GPU=gl BROOK_GPU_DISPLAY=sdl BROOK_COMPOSITE=gpu ./scripts/run-qemu.sh --release --script wayland_yquake2_gl"
