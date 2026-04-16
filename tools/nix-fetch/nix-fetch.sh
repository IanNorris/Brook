#!/bin/sh
# nix-fetch: Download and install a Nix package from cache.nixos.org
#
# Usage:
#   nix-fetch <store-path-hash>          # e.g. 10s5j3mfdg22k1597x580qrhprnzcjwb
#   nix-fetch <full-store-path>          # e.g. /nix/store/10s5j3mfdg22k1597x580qrhprnzcjwb-hello-2.12.3
#   nix-fetch --deps <store-path-hash>   # also fetch all dependencies
#
# Requires: curl, xz, nar-unpack (all in /nix/store or /bin)

set -e

CACHE_URL="${CACHE_URL:-https://cache.nixos.org}"
STORE_DIR="${STORE_DIR:-/nix/store}"

# Find our tools
CURL="${CURL:-$(command -v curl 2>/dev/null)}"
XZ="${XZ:-$(command -v xz 2>/dev/null)}"
NAR_UNPACK="${NAR_UNPACK:-$(command -v nar-unpack 2>/dev/null)}"

if [ -z "$CURL" ] || [ -z "$XZ" ] || [ -z "$NAR_UNPACK" ]; then
    echo "nix-fetch: missing required tools" >&2
    echo "  curl:       ${CURL:-NOT FOUND}" >&2
    echo "  xz:         ${XZ:-NOT FOUND}" >&2
    echo "  nar-unpack: ${NAR_UNPACK:-NOT FOUND}" >&2
    exit 1
fi

FETCH_DEPS=0
if [ "$1" = "--deps" ]; then
    FETCH_DEPS=1
    shift
fi

if [ -z "$1" ]; then
    echo "Usage: nix-fetch [--deps] <store-path-hash|full-store-path>" >&2
    exit 1
fi

# Extract hash from full store path or use as-is
INPUT="$1"
HASH=$(echo "$INPUT" | sed 's|^/nix/store/||' | cut -d- -f1)

if [ ${#HASH} -ne 32 ]; then
    echo "nix-fetch: invalid store path hash: $HASH" >&2
    exit 1
fi

fetch_package() {
    local hash="$1"

    # Fetch narinfo
    echo "Fetching narinfo for ${hash}..."
    local narinfo
    narinfo=$($CURL -4 -sL "${CACHE_URL}/${hash}.narinfo" 2>/dev/null)

    if [ -z "$narinfo" ]; then
        echo "nix-fetch: narinfo not found for ${hash}" >&2
        return 1
    fi

    # Parse narinfo fields
    local store_path nar_url compression references
    store_path=$(echo "$narinfo" | grep '^StorePath:' | cut -d' ' -f2)
    nar_url=$(echo "$narinfo" | grep '^URL:' | cut -d' ' -f2)
    compression=$(echo "$narinfo" | grep '^Compression:' | cut -d' ' -f2)
    references=$(echo "$narinfo" | grep '^References:' | cut -d' ' -f2-)

    if [ -z "$store_path" ] || [ -z "$nar_url" ]; then
        echo "nix-fetch: invalid narinfo for ${hash}" >&2
        return 1
    fi

    local dest="${STORE_DIR}/$(basename "$store_path")"

    # Skip if already installed
    if [ -e "$dest" ]; then
        echo "  Already installed: $(basename "$store_path")"
        return 0
    fi

    echo "  Package:     $(basename "$store_path")"
    echo "  Compression: ${compression:-none}"

    # Fetch dependencies first if requested
    if [ "$FETCH_DEPS" -eq 1 ] && [ -n "$references" ]; then
        for ref in $references; do
            local ref_hash=$(echo "$ref" | cut -d- -f1)
            local ref_path="${STORE_DIR}/${ref}"
            if [ ! -e "$ref_path" ]; then
                echo "  Fetching dependency: ${ref}"
                fetch_package "$ref_hash" || echo "  Warning: failed to fetch dep ${ref}"
            fi
        done
    fi

    # Download and extract
    echo "  Downloading NAR..."
    local full_url="${CACHE_URL}/${nar_url}"

    case "${compression:-none}" in
        xz)
            $CURL -4 -sL "$full_url" | $XZ -d | $NAR_UNPACK "$dest"
            ;;
        none)
            $CURL -4 -sL "$full_url" | $NAR_UNPACK "$dest"
            ;;
        *)
            echo "nix-fetch: unsupported compression: ${compression}" >&2
            return 1
            ;;
    esac

    echo "  Installed: $(basename "$store_path")"
}

fetch_package "$HASH"
