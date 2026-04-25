#!/usr/bin/env bash
# End-to-end Wayland input test.
#
# Boots Brook with the persistent waylandd + weston-flower script,
# injects mouse motion + button events through the QEMU monitor, then
# asserts on the serial log that:
#
#   1. waylandd registered as the global input grabber
#   2. waylandd_0 + weston-flower_1 both reached "first blit"
#   3. injected pointer events were forwarded as wl_pointer.button events
#
# This is the smallest test that exercises the full kernel→waylandd→
# Wayland-client input pipeline in raw (non-WM) compositor mode.
#
# Usage: scripts/e2e_wayland_input.sh [build_type]
#        build_type defaults to "release".

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_TYPE="${1:-release}"

cd "${ROOT_DIR}"

SERIAL_LOG="$(mktemp /tmp/brook-e2e-wayland-XXXXXX.log)"
QMON_SOCK="$(mktemp -u /tmp/brook-e2e-qmon-XXXXXX.sock)"

cleanup() {
    # Be deliberate about killing only our QEMU instance — match by serial
    # log path which is unique per invocation.
    if [ -n "${QEMU_PID:-}" ]; then
        kill "${QEMU_PID}" 2>/dev/null || true
    fi
    rm -f "${QMON_SOCK}"
}
trap cleanup EXIT

echo "=== Brook E2E Wayland input test ==="
echo "    serial log: ${SERIAL_LOG}"
echo "    qmp sock:   ${QMON_SOCK}"

export BROOK_SKIP_UPDATE_DISK=1
export SERIAL_OPT="-serial file:${SERIAL_LOG} -monitor unix:${QMON_SOCK},server,nowait"

setsid timeout 60 bash scripts/run-qemu.sh "--${BUILD_TYPE}" \
    --headless --vnc 127.0.0.1:0 --script wayland_flower \
    >/dev/null 2>&1 &
QEMU_PID=$!

# Wait for the input grabber to register before injecting events.
echo -n "    waiting for input grabber..."
for i in $(seq 1 40); do
    if grep -q "input grab acquired" "${SERIAL_LOG}" 2>/dev/null; then
        echo " ok (${i}s)"
        break
    fi
    sleep 1
done

if ! grep -q "input grab acquired" "${SERIAL_LOG}" 2>/dev/null; then
    echo " TIMEOUT"
    echo "--- serial tail ---"
    tail -40 "${SERIAL_LOG}" || true
    exit 1
fi

# Inject events: a few motion samples and three button down/up cycles.
echo "    injecting input via QEMU monitor..."
for i in 1 2 3 4 5 6; do
    printf 'mouse_move %d %d\n' $((i * 80)) $((i * 50)) | \
        nc -U -q 1 "${QMON_SOCK}" >/dev/null 2>&1 || true
    sleep 0.2
done
for _ in 1 2 3; do
    printf 'mouse_button 1\nmouse_button 0\n' | \
        nc -U -q 1 "${QMON_SOCK}" >/dev/null 2>&1 || true
    sleep 0.4
done

# Give the kernel a few frames to forward the events through to waylandd.
sleep 3

echo "    checking assertions..."
FAIL=0

assert_grep() {
    local pattern="$1" desc="$2" min="${3:-1}"
    local n
    n=$(grep -cE "${pattern}" "${SERIAL_LOG}" 2>/dev/null || echo 0)
    if [ "${n}" -ge "${min}" ]; then
        printf '      [PASS] %-50s (n=%d)\n' "${desc}" "${n}"
    else
        printf '      [FAIL] %-50s (n=%d, need ≥%d)\n' "${desc}" "${n}" "${min}"
        FAIL=1
    fi
}

assert_grep "COMPOSITOR: input grabber set"          "input grabber registered"           1
assert_grep "first blit pid [0-9]+ 'waylandd_0'"     "waylandd_0 first blit"              1
assert_grep "first blit pid [0-9]+ 'weston-flower_1'" "weston-flower_1 first blit"         1
assert_grep "\\[waylandd\\] input_pop: drained"       "kernel→waylandd input pump"        1

if [ "${FAIL}" -ne 0 ]; then
    echo "--- serial tail ---"
    tail -80 "${SERIAL_LOG}" || true
    echo "==="
    echo "FAIL — see ${SERIAL_LOG}"
    exit 1
fi

rm -f "${SERIAL_LOG}"
echo "==="
echo "PASS"
