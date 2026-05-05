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
    echo -n "/nix" > "${MNTDIR}/BROOK.MNT"
    mkdir -p "${MNTDIR}/bin"
    WAYLANDD_OUT=""
    if command -v nix-build &>/dev/null && command -v nix-store &>/dev/null; then
        WAYLANDD_OUT=$(nix-build "${ROOT_DIR}/tools/waylandd-pkg" --no-out-link)
        mkdir -p "${MNTDIR}/store"
        while IFS= read -r p; do
            [ -n "$p" ] || continue
            base=$(basename "$p")
            dst="${MNTDIR}/store/${base}"
            if [ ! -e "$dst" ]; then
                cp -a --no-preserve=links "$p" "$dst"
                echo "  store/${base}"
            fi
        done < <(nix-store -qR "$WAYLANDD_OUT")

        # Copy ffplay (ffmpeg-bin) closure for video playback
        FFPLAY_STORE="/nix/store/nih4c7knfj2mgjz6jp5l0fqn86xdwmhc-ffmpeg-8.0.1-bin"
        if [ -x "${FFPLAY_STORE}/bin/ffplay" ]; then
            ADDED=0
            while IFS= read -r p; do
                [ -n "$p" ] || continue
                base=$(basename "$p")
                dst="${MNTDIR}/store/${base}"
                if [ ! -e "$dst" ]; then
                    cp -a --no-preserve=links "$p" "$dst"
                    ADDED=$((ADDED + 1))
                fi
            done < <(nix-store -qR "$FFPLAY_STORE")
            cp "${FFPLAY_STORE}/bin/ffplay" "${MNTDIR}/bin/ffplay"
            echo "  ffplay -> /nix/bin/ffplay ($ADDED new store paths)"
        fi

        # Copy brook-player (minimal wl_shm video player)
        BROOK_PLAYER_STORE="/nix/store/gjardwnmds8s5ww9qyq4j44i4hbiaqmc-brook-player-0.1-brook"
        if [ -x "${BROOK_PLAYER_STORE}/bin/brook-player" ]; then
            ADDED=0
            while IFS= read -r p; do
                [ -n "$p" ] || continue
                base=$(basename "$p")
                dst="${MNTDIR}/store/${base}"
                if [ ! -e "$dst" ]; then
                    cp -a --no-preserve=links "$p" "$dst"
                    ADDED=$((ADDED + 1))
                fi
            done < <(nix-store -qR "$BROOK_PLAYER_STORE")
            cp "${BROOK_PLAYER_STORE}/bin/brook-player" "${MNTDIR}/bin/brook-player"
            echo "  brook-player -> /nix/bin/brook-player ($ADDED new store paths)"
        fi
    fi
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
    # Ensure nix -> nix-install symlink exists. Older images may have a
    # stale /nix/bin/nix directory, which ln -sf will not replace.
    rm -rf "${MNTDIR}/bin/nix"
    ln -sfnT nix-install "${MNTDIR}/bin/nix"

    # Bundle curl, xz binaries and CA certs from the on-disk store so
    # nix-fetch finds them at stable paths (/nix/bin/curl, /nix/bin/xz,
    # /nix/etc/ssl/certs/ca-bundle.crt). Without these, `nix install`
    # fails immediately with "curl not found".
    mkdir -p "${MNTDIR}/etc"
    cp "${ROOT_DIR}/data/asound.conf" "${MNTDIR}/etc/asound.conf"
    echo "  asound.conf -> /nix/etc/asound.conf"

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
        if [ -n "$WAYLANDD_OUT" ] && [ -x "${MNTDIR}/store/$(basename "$WAYLANDD_OUT")/bin/waylandd" ]; then
            cp "${MNTDIR}/store/$(basename "$WAYLANDD_OUT")/bin/waylandd" "${MNTDIR}/bin/waylandd"
            echo "  waylandd -> /nix/bin/waylandd"
        else
            for d in "${MNTDIR}"/store/*-waylandd-brook-*; do
                [ -x "$d/bin/waylandd" ] || continue
                cp "$d/bin/waylandd" "${MNTDIR}/bin/waylandd"
                echo "  waylandd -> /nix/bin/waylandd"
                break
            done
        fi
        for d in "${MNTDIR}"/store/*-gimp-*; do
            [ -x "$d/bin/gimp" ] || continue
            ln -sf "../store/$(basename "$d")/bin/gimp" "${MNTDIR}/bin/gimp"

            lite_dir="${MNTDIR}/share/gimp-lite"
            rm -rf "${lite_dir}"
            mkdir -p "${lite_dir}/plug-ins"
            for plugin in file-png file-jpeg file-bmp file-tiff file-webp; do
                if [ -d "$d/lib/gimp/3.0/plug-ins/${plugin}" ]; then
                    cp -a "$d/lib/gimp/3.0/plug-ins/${plugin}" \
                        "${lite_dir}/plug-ins/${plugin}"
                fi
            done
            cat > "${lite_dir}/gimprc" <<'EOF'
(plug-in-path "/nix/share/gimp-lite/plug-ins")
(pluginrc-path "/home/gimp-lite-pluginrc")
(module-path "")
(interpreter-path "")
(environ-path "")
(save-document-history no)
(save-session-info no)
(num-processors 1)
EOF
            batch_dir="${MNTDIR}/share/gimp-batch"
            rm -rf "${batch_dir}"
            mkdir -p "${batch_dir}/plug-ins"
            for plugin in file-png file-jpeg file-bmp file-tiff file-webp script-fu; do
                if [ -d "$d/lib/gimp/3.0/plug-ins/${plugin}" ]; then
                    cp -a "$d/lib/gimp/3.0/plug-ins/${plugin}" \
                        "${batch_dir}/plug-ins/${plugin}"
                fi
            done
            cat > "${batch_dir}/gimprc" <<'EOF'
(plug-in-path "/nix/share/gimp-batch/plug-ins")
(pluginrc-path "/home/gimp-batch-pluginrc")
(module-path "")
(interpreter-path "")
(environ-path "")
(save-document-history no)
(save-session-info no)
(num-processors 1)
EOF
            echo "  gimp -> /nix/bin/gimp"
            echo "  gimp-lite gimprc + file plug-ins -> /nix/share/gimp-lite"
            echo "  gimp-batch gimprc + script-fu -> /nix/share/gimp-batch"
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
        # Remove vaapi_filters plugin from VLC — Brook has no GPU and this
        # module causes VAOP format negotiation that fails after window resize.
        for f in "${MNTDIR}"/store/*-vlc-*/lib/vlc/plugins/vaapi/libvaapi_filters_plugin.so; do
            [ -f "$f" ] || continue
            rm -f "$f"
            echo "  removed libvaapi_filters_plugin.so (no GPU)"
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
