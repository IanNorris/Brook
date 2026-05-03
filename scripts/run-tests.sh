#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_TYPE_INPUT="${1:-debug}"
BUILD_TYPE="$(echo "${BUILD_TYPE_INPUT}" | tr '[:upper:]' '[:lower:]')"
BUILD_DIR="${ROOT_DIR}/build/${BUILD_TYPE}"

# Locate OVMF firmware (same logic as run-qemu.sh)
OVMF_CODE="${OVMF_CODE:-}"
OVMF_VARS="${OVMF_VARS:-}"

if [ -z "${OVMF_CODE}" ]; then
    OVMF_SEARCH_PATHS=(
        "/run/current-system/sw/share/OVMF"
        "/nix/var/nix/profiles/default/share/OVMF"
        "/usr/share/OVMF"
        "/usr/share/ovmf"
        "/usr/share/edk2-ovmf/x64"
        "/usr/share/edk2/ovmf"
    )

    for dir in "${OVMF_SEARCH_PATHS[@]}"; do
        if [ -f "${dir}/OVMF_CODE.fd" ]; then
            OVMF_CODE="${dir}/OVMF_CODE.fd"
            OVMF_VARS="${dir}/OVMF_VARS.fd"
            break
        elif [ -f "${dir}/OVMF.fd" ]; then
            OVMF_CODE="${dir}/OVMF.fd"
            OVMF_VARS="${dir}/OVMF.fd"
            break
        fi
    done
fi

if [ -z "${OVMF_CODE}" ] && command -v nix-build &>/dev/null; then
    echo "Locating OVMF via nix-build..."
    OVMF_STORE="$(nix-build '<nixpkgs>' -A OVMF.fd --no-out-link 2>/dev/null || true)"
    if [ -n "${OVMF_STORE}" ] && [ -f "${OVMF_STORE}/FV/OVMF_CODE.fd" ]; then
        OVMF_CODE="${OVMF_STORE}/FV/OVMF_CODE.fd"
        OVMF_VARS="${OVMF_STORE}/FV/OVMF_VARS.fd"
    fi
fi

if [ -z "${OVMF_CODE}" ]; then
    echo "Error: OVMF firmware not found."
    echo "Set OVMF_CODE=/path/to/OVMF_CODE.fd or run from nix-shell."
    exit 1
fi

# Set up test ESP
TEST_ESP_DIR="${BUILD_DIR}/esp-tests"
BOOTLOADER="${BUILD_DIR}/bootloader/BOOTX64.efi"

if [ ! -f "${BOOTLOADER}" ]; then
    echo "Bootloader not found at ${BOOTLOADER}"
    echo "Run scripts/build.sh first."
    exit 1
fi

mkdir -p "${TEST_ESP_DIR}/EFI/BOOT"
mkdir -p "${TEST_ESP_DIR}/TESTS"
cp "${BOOTLOADER}" "${TEST_ESP_DIR}/EFI/BOOT/BOOTX64.EFI"

PASS_COUNT=0
FAIL_COUNT=0
FAILED_TESTS=()

run_test() {
    local TEST_NAME="$1"
    local TEST_ELF="${BUILD_DIR}/tests/${TEST_NAME}.elf"

    if [ ! -f "${TEST_ELF}" ]; then
        echo "[SKIP] ${TEST_NAME} (not found)"
        return
    fi

    cp "${TEST_ELF}" "${TEST_ESP_DIR}/TESTS/${TEST_NAME^^}.ELF"

    # Write BROOK.CFG pointing at this test binary
    cat > "${TEST_ESP_DIR}/BROOK.CFG" <<EOF
# Brook OS test configuration
TARGET=TESTS\\${TEST_NAME^^}.ELF
DEBUG_TEXT=0
EOF

    # Copy OVMF VARS to a writable temporary location
    local VARS_COPY SERIAL_LOG
    VARS_COPY="$(mktemp /tmp/brook-test-vars-XXXXXX.fd)"
    SERIAL_LOG="$(mktemp /tmp/brook-test-serial-XXXXXX.log)"
    cp "${OVMF_VARS}" "${VARS_COPY}"

    local QEMU_EXIT=0
    # Use -serial file: instead of -serial stdio to avoid QEMU blocking its
    # event loop when stdin is not a TTY (pipe/script context).
    # Serial output is captured to SERIAL_LOG and printed on failure.
    timeout 30 qemu-system-x86_64 \
        -machine q35 \
        -cpu qemu64 \
        -m 256M \
        -drive if=pflash,format=raw,readonly=on,file="${OVMF_CODE}" \
        -drive if=pflash,format=raw,file="${VARS_COPY}" \
        -drive format=raw,file=fat:rw:"${TEST_ESP_DIR}" \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -serial "file:${SERIAL_LOG}" \
        -display none \
        -monitor none \
        2>/dev/null || QEMU_EXIT=$?

    rm -f "${VARS_COPY}"

    # isa-debug-exit: write N → QEMU exits with 2*N+1
    # We write 0 for pass → exit 1
    # We write 1 for fail → exit 3
    # timeout exits with 124 if test hangs
    if [ "${QEMU_EXIT}" -eq 1 ]; then
        echo "[PASS] ${TEST_NAME}"
        PASS_COUNT=$((PASS_COUNT + 1))
    elif [ "${QEMU_EXIT}" -eq 124 ]; then
        echo "[TIMEOUT] ${TEST_NAME}"
        echo "--- serial output ---"
        cat "${SERIAL_LOG}" 2>/dev/null || true
        echo "---------------------"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        FAILED_TESTS+=("${TEST_NAME} (timeout)")
    else
        echo "[FAIL] ${TEST_NAME} (exit ${QEMU_EXIT})"
        echo "--- serial output ---"
        cat "${SERIAL_LOG}" 2>/dev/null || true
        echo "---------------------"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        FAILED_TESTS+=("${TEST_NAME}")
    fi

    rm -f "${SERIAL_LOG}"
}

# Discover and run all test ELFs
found_any=0
for test_elf in "${BUILD_DIR}/tests/"*.elf; do
    [ -f "${test_elf}" ] || continue
    found_any=1
    TEST_NAME="$(basename "${test_elf}" .elf)"
    run_test "${TEST_NAME}"
done

if [ "${found_any}" -eq 0 ]; then
    echo "No test ELFs found in ${BUILD_DIR}/tests/"
    exit 1
fi

echo ""
echo "Results: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"

if [ ${FAIL_COUNT} -gt 0 ]; then
    echo "Failed tests:"
    for t in "${FAILED_TESTS[@]}"; do
        echo "  - $t"
    done
    exit 1
fi
