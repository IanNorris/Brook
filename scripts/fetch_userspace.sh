#!/usr/bin/env bash
# Fetch userspace binaries from Nix for the Brook OS disk image.
#
# Downloads busybox (static musl), bash (dynamic musl), and musl dynamic
# libraries into the repo root where update_disk.sh expects them.
#
# Usage:
#   scripts/fetch_userspace.sh
#
# Requires: nix-shell (part of Nix package manager)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

echo "Fetching userspace binaries from Nix..."

# --- Busybox (static musl) ---
BUSYBOX_OUT="${ROOT_DIR}/busybox_static"
if [ ! -f "$BUSYBOX_OUT" ]; then
    echo "Fetching busybox (static musl)..."
    BUSYBOX_PKG=$(nix-build '<nixpkgs>' -A pkgsCross.musl64.busybox-sandbox-shell --no-out-link 2>/dev/null || \
                  nix-build '<nixpkgs>' -A pkgsStatic.busybox --no-out-link 2>/dev/null || true)
    if [ -z "$BUSYBOX_PKG" ]; then
        # Fallback: search nix store for existing busybox
        BUSYBOX_PKG=$(find /nix/store -maxdepth 1 -name '*busybox-static*musl*' -type d 2>/dev/null | head -1)
    fi
    if [ -n "$BUSYBOX_PKG" ] && [ -f "$BUSYBOX_PKG/bin/busybox" ]; then
        cp "$BUSYBOX_PKG/bin/busybox" "$BUSYBOX_OUT"
        echo "  → busybox_static ($(stat -c%s "$BUSYBOX_OUT") bytes)"
    else
        echo "  ⚠ busybox not found in nix store, skipping"
    fi
else
    echo "  busybox_static already exists ($(stat -c%s "$BUSYBOX_OUT") bytes)"
fi

# --- Bash (dynamic musl) ---
BASH_OUT="${ROOT_DIR}/bash_dynamic"
if [ ! -f "$BASH_OUT" ]; then
    echo "Fetching bash (dynamic musl)..."
    BASH_PKG=$(nix-build '<nixpkgs>' -A pkgsCross.musl64.bash --no-out-link 2>/dev/null || true)
    if [ -n "$BASH_PKG" ] && [ -f "$BASH_PKG/bin/bash" ]; then
        cp "$BASH_PKG/bin/bash" "$BASH_OUT"
        echo "  → bash_dynamic ($(stat -c%s "$BASH_OUT") bytes)"
    else
        # Search nix store
        BASH_BIN=$(find /nix/store -path '*/bash-*musl*/bin/bash' -type f 2>/dev/null | head -1)
        if [ -n "$BASH_BIN" ]; then
            cp "$BASH_BIN" "$BASH_OUT"
            echo "  → bash_dynamic ($(stat -c%s "$BASH_OUT") bytes)"
        else
            echo "  ⚠ bash (musl) not found, skipping"
        fi
    fi
else
    echo "  bash_dynamic already exists ($(stat -c%s "$BASH_OUT") bytes)"
fi

# --- Dynamic libraries (musl libc, readline, ncurses) ---
DYNLIBS_DIR="${ROOT_DIR}/dynlibs"
mkdir -p "$DYNLIBS_DIR"

fetch_libs() {
    local name="$1"
    local nix_attr="$2"
    local lib_subdir="${3:-lib}"

    echo "Fetching $name libs..."
    local pkg
    pkg=$(nix-build '<nixpkgs>' -A "$nix_attr" --no-out-link 2>/dev/null || true)
    if [ -z "$pkg" ]; then
        # Search nix store
        local pattern="$4"
        if [ -n "${pattern:-}" ]; then
            pkg=$(find /nix/store -maxdepth 1 -name "$pattern" -type d 2>/dev/null | head -1)
        fi
    fi
    if [ -n "$pkg" ] && [ -d "$pkg/$lib_subdir" ]; then
        for f in "$pkg/$lib_subdir"/lib*.so*; do
            [ -f "$f" ] || continue
            local base
            base=$(basename "$f")
            # Follow symlinks to get the actual file
            if [ -L "$f" ]; then
                local target
                target=$(readlink -f "$f")
                cp "$target" "$DYNLIBS_DIR/$base"
            else
                cp "$f" "$DYNLIBS_DIR/$base"
            fi
        done
        echo "  → $name libs copied"
    else
        echo "  ⚠ $name not found, skipping"
    fi
}

# musl libc
MUSL_LIB=$(find /nix/store -maxdepth 2 -name 'ld-musl-x86_64.so.1' -path '*musl*' 2>/dev/null | head -1)
if [ -n "$MUSL_LIB" ]; then
    MUSL_DIR=$(dirname "$MUSL_LIB")
    for f in "$MUSL_DIR"/lib*.so* "$MUSL_DIR"/ld-*.so*; do
        [ -f "$f" ] || continue
        base=$(basename "$f")
        if [ -L "$f" ]; then
            cp "$(readlink -f "$f")" "$DYNLIBS_DIR/$base"
        else
            cp "$f" "$DYNLIBS_DIR/$base"
        fi
    done
    echo "  → musl libc libs copied"
else
    fetch_libs "musl" "pkgsCross.musl64.musl" "lib" "*musl*x86_64*"
fi

# readline
fetch_libs "readline" "pkgsCross.musl64.readline" "lib" "*readline*musl*"

# ncurses
fetch_libs "ncurses" "pkgsCross.musl64.ncurses" "lib" "*ncurses*musl*"

echo ""
echo "Userspace binaries:"
[ -f "$BUSYBOX_OUT" ] && echo "  busybox_static  $(stat -c%s "$BUSYBOX_OUT") bytes"
[ -f "$BASH_OUT" ]    && echo "  bash_dynamic    $(stat -c%s "$BASH_OUT") bytes"
echo "  dynlibs/:"
ls -la "$DYNLIBS_DIR"/*.so* 2>/dev/null | awk '{print "    " $NF " (" $5 " bytes)"}' || echo "    (none)"
echo ""
echo "Run scripts/update_disk.sh to sync these to the disk image."
