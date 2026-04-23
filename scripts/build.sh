#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_TYPE="${1:-Debug}"
BUILD_TYPE_LOWER="$(echo "${BUILD_TYPE}" | tr '[:upper:]' '[:lower:]')"
BUILD_DIR="${ROOT_DIR}/build/${BUILD_TYPE_LOWER}"

echo "Brook OS Build — ${BUILD_TYPE}"
echo "Root: ${ROOT_DIR}"
echo "Build dir: ${BUILD_DIR}"

# Initialize submodules if needed
if [ ! -f "${ROOT_DIR}/vendor/uefi-headers/Include/Uefi.h" ] || \
   [ ! -f "${ROOT_DIR}/src/third_party/fatfs/source/ff.c" ]; then
    echo "Initializing submodules..."
    git -C "${ROOT_DIR}" submodule update --init --recursive
fi

mkdir -p "${BUILD_DIR}"

# If the cache was built from a different source directory (e.g. different
# machine or workspace), wipe it so CMake doesn't error out.
CACHE_FILE="${BUILD_DIR}/CMakeCache.txt"
if [ -f "${CACHE_FILE}" ]; then
    CACHED_SRC=$(grep "^CMAKE_HOME_DIRECTORY" "${CACHE_FILE}" 2>/dev/null | cut -d= -f2)
    if [ -n "${CACHED_SRC}" ] && [ "${CACHED_SRC}" != "${ROOT_DIR}" ]; then
        echo "CMake cache path mismatch (cached: ${CACHED_SRC}, current: ${ROOT_DIR})"
        echo "Wiping build directory..."
        rm -rf "${BUILD_DIR}"
        mkdir -p "${BUILD_DIR}"
    fi
fi

echo "Configuring..."
cmake \
    -S "${ROOT_DIR}" \
    -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -G Ninja

echo "Building..."
cmake --build "${BUILD_DIR}"

# Build host-native tests (uses nix-wrapped clang++ with standard host libs).
# HOST_CXX is set by shell.nix to the wrapped Clang+libc++ toolchain;
# CC/CXX remain the unwrapped Clang used for kernel cross-compilation.
echo "Building host tests..."
HOST_TEST_DIR="${BUILD_DIR}/host_tests"
mkdir -p "${HOST_TEST_DIR}"
HOST_COMPILER="${HOST_CXX:-${CXX:-c++}}"
if cmake \
    -S "${ROOT_DIR}/src/tests/host" \
    -B "${HOST_TEST_DIR}" \
    -DCMAKE_CXX_COMPILER="${HOST_COMPILER}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DBROOK_ROOT="${ROOT_DIR}" \
    -G Ninja >/dev/null 2>&1
then
    if cmake --build "${HOST_TEST_DIR}" >/dev/null 2>&1; then
        echo "Host tests built."
    else
        echo "Host tests skipped (build failed — rerun with verbose to debug)."
    fi
else
    echo "Host tests skipped (configure failed — likely stale CMake cache; rm -rf ${HOST_TEST_DIR} to fix)."
fi

echo "Done. Artifacts in ${BUILD_DIR}/"
