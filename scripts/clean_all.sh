#!/usr/bin/env bash
# Clean all build artifacts and optionally the disk image.
# Usage: scripts/clean_all.sh [--disk]   # --disk also removes brook_disk.img
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

CLEAN_DISK=0
for arg in "$@"; do
    [ "$arg" = "--disk" ] && CLEAN_DISK=1
done

echo "=== Brook OS — Clean ==="

# Remove build directory
if [ -d "${ROOT_DIR}/build" ]; then
    echo "Removing build/..."
    rm -rf "${ROOT_DIR}/build"
fi

# Remove disk image if requested
DISK_IMG="${BROOK_DISK_IMG:-${ROOT_DIR}/brook_disk.img}"
if [ "${CLEAN_DISK}" -eq 1 ] && [ -f "${DISK_IMG}" ]; then
    echo "Removing disk image: ${DISK_IMG}"
    rm -f "${DISK_IMG}"
fi

echo "Clean complete."
echo ""
echo "Rebuild with: nix-shell --run 'scripts/build_all.sh [Debug|Release]'"
