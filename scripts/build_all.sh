#!/usr/bin/env bash
# Build everything: kernel, bootloader, tests, DOOM, and update disk image.
# Usage: scripts/build_all.sh [Debug|Release]   (default: Debug)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_TYPE="${1:-Debug}"

echo "=== Brook OS — Full Build (${BUILD_TYPE}) ==="
echo ""

# 1. Kernel + bootloader + kernel tests
echo "--- Kernel & Bootloader ---"
"${SCRIPT_DIR}/build.sh" "${BUILD_TYPE}"
echo ""

# 2. DOOM (if doomgeneric source is available)
DOOM_SRC="${DOOM_SRC:-}"
if [ -z "$DOOM_SRC" ]; then
    # Check common locations relative to the repo
    for candidate in \
        "${ROOT_DIR}/../doomgeneric_enkel/doomgeneric" \
        "${ROOT_DIR}/vendor/doomgeneric_enkel/doomgeneric"; do
        if [ -d "$candidate" ]; then
            DOOM_SRC="$(cd "$candidate" && pwd)"
            break
        fi
    done
fi

if [ -n "$DOOM_SRC" ] && [ -d "$DOOM_SRC" ]; then
    echo "--- DOOM ---"
    DOOM_SRC="$DOOM_SRC" "${SCRIPT_DIR}/build_doom.sh"
    echo ""
else
    echo "--- DOOM (skipped — doomgeneric source not found) ---"
    echo "  Set DOOM_SRC=/path/to/doomgeneric_enkel/doomgeneric to enable."
    echo ""
fi

# 3. Userspace test apps (requires musl cross-compiler)
if command -v x86_64-unknown-linux-musl-gcc &>/dev/null; then
    echo "--- Apps ---"
    "${SCRIPT_DIR}/build_apps.sh"
    echo ""
else
    echo "--- Apps (skipped — x86_64-unknown-linux-musl-gcc not found) ---"
    echo ""
fi

# 4. Update disk image
echo "--- Disk Image ---"
"${SCRIPT_DIR}/update_disk.sh" --create
echo ""

# 4. Run host tests
BUILD_TYPE_LOWER="$(echo "${BUILD_TYPE}" | tr '[:upper:]' '[:lower:]')"
HOST_TEST_DIR="${ROOT_DIR}/build/${BUILD_TYPE_LOWER}/host_tests"
if [ -d "${HOST_TEST_DIR}" ]; then
    echo "--- Host Tests ---"
    (cd "${HOST_TEST_DIR}" && ctest --output-on-failure) || true
    echo ""
fi

echo "=== Build complete ==="
echo "Run with: scripts/run-qemu.sh $([ "$BUILD_TYPE" = "Release" ] && echo "--release" || true)"
