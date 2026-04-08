#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_TYPE="${1:-Debug}"
BUILD_DIR="${ROOT_DIR}/build"

echo "Brook OS Build — ${BUILD_TYPE}"
echo "Root: ${ROOT_DIR}"

# Initialize submodules if needed
if [ ! -f "${ROOT_DIR}/vendor/uefi-headers/Include/Uefi.h" ]; then
    echo "Initializing submodules..."
    git -C "${ROOT_DIR}" submodule update --init --recursive
fi

mkdir -p "${BUILD_DIR}"

echo "Configuring..."
cmake \
    -S "${ROOT_DIR}" \
    -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -G Ninja

echo "Building..."
cmake --build "${BUILD_DIR}"

echo "Done. Artifacts in ${BUILD_DIR}/"
