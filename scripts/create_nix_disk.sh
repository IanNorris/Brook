#!/usr/bin/env bash
# Create an ext2 disk image pre-populated with Nix store closures.
#
# Supports both nixpkgs attributes and local derivations:
#   scripts/create_nix_disk.sh [size_mb] [nix_attr_or_path...]
#
# Examples:
#   scripts/create_nix_disk.sh 256                        # defaults
#   scripts/create_nix_disk.sh 256 bash curl.bin          # nixpkgs attrs
#   scripts/create_nix_disk.sh 256 ./tools/netsurf-pkg    # local derivation
#
# Default packages include bash, curl, xz, coreutils, nss-cacert, and
# the Brook NetSurf package.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

# Parse size (first numeric arg) and packages
SIZE_MB=256
PACKAGES=()
for arg in "$@"; do
    if [[ "$arg" =~ ^[0-9]+$ ]]; then
        SIZE_MB="$arg"
    else
        PACKAGES+=("$arg")
    fi
done

# Default package set for Brook OS
if [ ${#PACKAGES[@]} -eq 0 ]; then
    PACKAGES=(
        bash
        curl.bin
        xz.bin
        cacert
        coreutils
        openssl         # openssl s_client for TLS handshake diagnosis
        iperf3          # TCP throughput + sequential connection stress testing
        netcat-openbsd  # raw TCP without TLS overhead (nc host port)
        "${ROOT_DIR}/tools/netsurf-pkg"
    )
fi

DISK_IMG="${BROOK_NIX_DISK:-${ROOT_DIR}/brook_nix_disk.img}"

echo "Creating ${SIZE_MB}MB Nix store disk at ${DISK_IMG}..."

# Create and format
dd if=/dev/zero of="${DISK_IMG}" bs=1M count="${SIZE_MB}" status=progress 2>&1
mkfs.ext2 -q -b 4096 -L "NIXSTORE" "${DISK_IMG}"

# Build all requested packages and collect closures
echo "Building packages: ${PACKAGES[*]}..."
ALL_PATHS=()
for pkg in "${PACKAGES[@]}"; do
    if [[ "$pkg" == /* || "$pkg" == ./* || "$pkg" == ../* ]]; then
        # Local derivation path
        PKG_PATH=$(nix-build "$pkg" --no-out-link)
    else
        # Nixpkgs attribute
        PKG_PATH=$(nix-build '<nixpkgs>' -A "$pkg" --no-out-link)
    fi
    if [ -z "$PKG_PATH" ]; then
        echo "  ERROR: nix-build returned empty path for '${pkg}'" >&2
        exit 1
    fi
    echo "  ${pkg}: ${PKG_PATH}"
    ALL_PATHS+=("$PKG_PATH")
done

# Get full transitive closure (deduplicated)
CLOSURE=$(nix-store -qR "${ALL_PATHS[@]}" | sort -u)
echo "  Closure ($(echo "$CLOSURE" | wc -l) paths):"
for p in ${CLOSURE}; do
    SIZE=$(du -sh "$p" | cut -f1)
    echo "    ${p}  (${SIZE})"
done

# Mount the disk image and populate it
MOUNT_DIR=$(mktemp -d)
echo "Mounting at ${MOUNT_DIR}..."

# Use fuse2fs if available (no root needed), otherwise debugfs
if command -v fuse2fs &>/dev/null; then
    fuse2fs -o rw "${DISK_IMG}" "${MOUNT_DIR}"
    FUSE=1
else
    echo "Warning: fuse2fs not available, using debugfs (slower)"
    FUSE=0
fi

# Build Brook nix tools (nix-search, nix-install, nix-fetch, nar-unpack)
echo "Building Brook Nix tools..."
TOOLS_BIN="${ROOT_DIR}/tools"
NIX_SEARCH="${TOOLS_BIN}/nix-search/nix-search"
NIX_INSTALL="${TOOLS_BIN}/nix-install/nix-install"
NIX_FETCH="${TOOLS_BIN}/nix-fetch/nix-fetch"
NAR_UNPACK="${TOOLS_BIN}/nar-unpack/nar-unpack"

for tool in "${NIX_SEARCH}" "${NIX_INSTALL}" "${NIX_FETCH}" "${NAR_UNPACK}"; do
    if [ ! -f "$tool" ]; then
        echo "  Warning: ${tool} not found, build it first"
    fi
done

# Generate package index
echo "Generating package index..."
INDEX_FILE="${ROOT_DIR}/tools/nix-index/packages.idx"
if [ ! -f "${INDEX_FILE}" ] || [ "${REGEN_INDEX:-0}" = "1" ]; then
    python3 "${ROOT_DIR}/tools/nix-index/gen-nix-index.py" --output "${INDEX_FILE}"
else
    echo "  Using existing index (set REGEN_INDEX=1 to regenerate)"
fi
echo "  Index: $(wc -l < "${INDEX_FILE}") packages, $(du -h "${INDEX_FILE}" | cut -f1)"

# Helper to write files and dirs via debugfs
write_to_disk() {
    local img="$1" src="$2" dst="$3"
    debugfs -w "$img" -R "write ${src} ${dst}" 2>/dev/null
}

mkdir_on_disk() {
    local img="$1" dir="$2"
    debugfs -w "$img" -R "mkdir ${dir}" 2>/dev/null
}

symlink_on_disk() {
    local img="$1" link="$2" target="$3"
    debugfs -w "$img" -R "symlink ${link} ${target}" 2>/dev/null
}

copy_tree_to_disk() {
    local img="$1" src="$2" dst="$3"
    mkdir_on_disk "$img" "$dst"

    (cd "$src" && find . -type d) | while read -r dir; do
        if [ "$dir" != "." ]; then
            mkdir_on_disk "$img" "${dst}/${dir#./}"
        fi
    done

    (cd "$src" && find . -type f) | while read -r file; do
        write_to_disk "$img" "${src}/${file#./}" "${dst}/${file#./}"
    done

    (cd "$src" && find . -type l) | while read -r link; do
        local target
        target=$(readlink "${src}/${link#./}")
        symlink_on_disk "$img" "${dst}/${link#./}" "${target}"
    done
}

if [ "${FUSE}" -eq 1 ]; then
    # Create BROOK.MNT
    echo -n "/nix" > "${MOUNT_DIR}/BROOK.MNT"

    # Create store directory
    mkdir -p "${MOUNT_DIR}/store"

    # Copy each store path
    for p in ${CLOSURE}; do
        BASENAME=$(basename "$p")
        echo "  Copying ${BASENAME}..."
        cp -a "$p" "${MOUNT_DIR}/store/${BASENAME}"
    done

    # Create /nix/bin with tools
    mkdir -p "${MOUNT_DIR}/bin"
    for tool in nix-fetch nix-search nix-install; do
        src="${TOOLS_BIN}/${tool}/${tool}"
        [ -f "$src" ] && cp "$src" "${MOUNT_DIR}/bin/${tool}"
    done
    [ -f "${NAR_UNPACK}" ] && cp "${NAR_UNPACK}" "${MOUNT_DIR}/bin/nar-unpack"
    # "nix" is a symlink to nix-install (which supports subcommands)
    ln -sf nix-install "${MOUNT_DIR}/bin/nix"

    # Create /nix/index with package index
    mkdir -p "${MOUNT_DIR}/index"
    cp "${INDEX_FILE}" "${MOUNT_DIR}/index/packages.idx"

    # Create /nix/profile directory
    mkdir -p "${MOUNT_DIR}/profile/bin"

    # Sync and unmount
    sync
    fusermount -u "${MOUNT_DIR}"
else
    # debugfs approach
    TMPFILE=$(mktemp)
    echo -n "/nix" > "${TMPFILE}"
    write_to_disk "${DISK_IMG}" "${TMPFILE}" "BROOK.MNT"
    rm -f "${TMPFILE}"

    mkdir_on_disk "${DISK_IMG}" "store"

    # Copy store paths
    for p in ${CLOSURE}; do
        BASENAME=$(basename "$p")
        echo "  Copying ${BASENAME}..."
        copy_tree_to_disk "${DISK_IMG}" "$p" "store/${BASENAME}"
    done

    # Create /nix/bin with tools
    mkdir_on_disk "${DISK_IMG}" "bin"
    for tool in nix-fetch nix-search nix-install; do
        src="${TOOLS_BIN}/${tool}/${tool}"
        [ -f "$src" ] && write_to_disk "${DISK_IMG}" "$src" "bin/${tool}"
    done
    [ -f "${NAR_UNPACK}" ] && write_to_disk "${DISK_IMG}" "${NAR_UNPACK}" "bin/nar-unpack"
    symlink_on_disk "${DISK_IMG}" "bin/nix" "nix-install"

    # Create /nix/index with package index
    mkdir_on_disk "${DISK_IMG}" "index"
    write_to_disk "${DISK_IMG}" "${INDEX_FILE}" "index/packages.idx"

    # Create /nix/profile directory
    mkdir_on_disk "${DISK_IMG}" "profile"
    mkdir_on_disk "${DISK_IMG}" "profile/bin"
fi

rmdir "${MOUNT_DIR}" 2>/dev/null || true

echo ""
echo "Nix store disk created: ${DISK_IMG}"
echo "  Mount point: /nix (via BROOK.MNT)"
echo "  Packages: ${PACKAGES[*]}"
echo "  Tools: nix, nix-search, nix-install, nix-fetch, nar-unpack"
echo "  Index: $(wc -l < "${INDEX_FILE}") packages"
for pkg_path in "${ALL_PATHS[@]}"; do
    echo "  Test: run ${pkg_path}/bin/..."
done
