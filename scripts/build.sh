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
    # Only these submodules are needed for the kernel build. Old optional
    # NetSurf gitlinks may have no entries in .gitmodules.
    git -C "${ROOT_DIR}" submodule update --init --recursive -- \
        vendor/uefi-headers src/third_party/fatfs
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

KERNEL_ELF="${BUILD_DIR}/kernel/BROOK.elf"
ENTRY_AUDIT="${BUILD_DIR}/entry-state-audit.json"
SCHED_AUDIT="${BUILD_DIR}/scheduler-start-audit.json"
AUDIT_STAMP="${BUILD_DIR}/kernel-audits.sha256"
PYTHON=$(command -v python3)
AUDIT_PYTHON=("${PYTHON}")

audit_inputs() {
    printf 'Interpreter: %s\nLibrary path: %s\n' "${PYTHON}" "${LD_LIBRARY_PATH:-}" || return
    sha256sum "${KERNEL_ELF}" "${SCRIPT_DIR}/build.sh" \
        "${SCRIPT_DIR}/audit_entry_state.py" "${SCRIPT_DIR}/audit_scheduler_start.py"
}

# Reuse only successful audits of the exact kernel and unchanged audit inputs.
# The dev shell's immutable Python environment pins Capstone and pyelftools.
# Isolated mode keeps Python's imports within that environment.
CACHE_AUDITS=false
NIX_LIB_DIRECTORY='^/nix/store/[0-9a-z]{32}-[^/:]+/lib$'
if [[ "${PYTHON}" == /nix/store/*-env/bin/python3 &&
      ( -z "${LD_LIBRARY_PATH:-}" || "${LD_LIBRARY_PATH}" =~ ${NIX_LIB_DIRECTORY} ) &&
      -z "${LD_PRELOAD:-}${LD_AUDIT:-}${LIBCAPSTONE_PATH:-}${NIX_PYTHONPATH:-}" ]]; then
    CACHE_AUDITS=true
    AUDIT_PYTHON+=(-I)
fi

if "${CACHE_AUDITS}"; then
    AUDIT_INPUTS=$(audit_inputs)
fi
if "${CACHE_AUDITS}" && [[ -s "${AUDIT_STAMP}" ]] &&
   AUDIT_OUTPUTS=$(sha256sum "${ENTRY_AUDIT}" "${SCHED_AUDIT}" 2>/dev/null) &&
   [[ "${AUDIT_INPUTS}"$'\n'"${AUDIT_OUTPUTS}" == "$(<"${AUDIT_STAMP}")" ]]; then
    echo "Entry and scheduler audits already passed for this kernel."
else
    rm -f "${AUDIT_STAMP}"
    echo "Auditing compiled entry/return templates..."
    "${AUDIT_PYTHON[@]}" "${SCRIPT_DIR}/audit_entry_state.py" \
        "${KERNEL_ELF}" --mutations \
        --output "${ENTRY_AUDIT}" >"${BUILD_DIR}/entry-state-audit.log"
    echo "Entry audit passed for supported templates (coverage: ${ENTRY_AUDIT})."
    "${AUDIT_PYTHON[@]}" "${SCRIPT_DIR}/audit_scheduler_start.py" \
        "${KERNEL_ELF}" --mutations \
        --output "${SCHED_AUDIT}" >"${BUILD_DIR}/scheduler-start-audit.log"
    echo "Scheduler startup saved-context call contract passed."
    if "${CACHE_AUDITS}" && [[ "${AUDIT_INPUTS}" == "$(audit_inputs)" ]]; then
        {
            printf '%s\n' "${AUDIT_INPUTS}"
            sha256sum "${ENTRY_AUDIT}" "${SCHED_AUDIT}"
        } >"${AUDIT_STAMP}"
    fi
fi

# Build host-native tests (uses nix-wrapped clang++ with standard host libs).
# HOST_CXX is set by shell.nix to the wrapped Clang+libc++ toolchain;
# CC/CXX remain the unwrapped Clang used for kernel cross-compilation.
echo "Building host tests..."
HOST_TEST_DIR="${BUILD_DIR}/host_tests"
mkdir -p "${HOST_TEST_DIR}"
HOST_COMPILER="${HOST_CXX:-${CXX:-c++}}"
HOST_C_COMPILER="${HOST_CC:-${CC:-cc}}"
if cmake \
    -S "${ROOT_DIR}/src/tests/host" \
    -B "${HOST_TEST_DIR}" \
    -DCMAKE_CXX_COMPILER="${HOST_COMPILER}" \
    -DCMAKE_C_COMPILER="${HOST_C_COMPILER}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DBROOK_ROOT="${ROOT_DIR}" \
    -G Ninja >"${HOST_TEST_DIR}/configure.log" 2>&1
then
    if cmake --build "${HOST_TEST_DIR}" >"${HOST_TEST_DIR}/build.log" 2>&1; then
        echo "Host tests built."
        echo "  Run host-native tests with: ctest --test-dir ${HOST_TEST_DIR} --output-on-failure"
    else
        echo "Host test build failed; see ${HOST_TEST_DIR}/build.log." >&2
        tail -25 "${HOST_TEST_DIR}/build.log" >&2
        exit 1
    fi
else
    echo "Host test configuration failed; see ${HOST_TEST_DIR}/configure.log." >&2
    tail -25 "${HOST_TEST_DIR}/configure.log" >&2
    exit 1
fi

echo "  Run bare-metal test ELFs with: ./scripts/run-tests.sh ${BUILD_TYPE_LOWER}"
echo "Done. Artifacts in ${BUILD_DIR}/"
