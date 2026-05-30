#!/usr/bin/env bash
#
# smoke-test.sh — scripted, asserted QEMU boot gate.
#
# Layer 3 of the deterministic test infrastructure. Turns the manual
# "boot Brook headless and eyeball the serial log" check into a single
# pass/fail command suitable for CI. Boots a real Brook image under QEMU,
# runs a named boot scenario, and asserts on serial output:
#   - every REQUIRED marker must appear  -> success
#   - any FORBIDDEN marker (fault/panic) -> immediate failure
#   - neither within the timeout         -> failure (hang)
#
# This guards the integration-level bug class from the architectural review
# (e.g. BRO-154: a missing module export only surfaced as a runtime #PF on a
# live boot, not in any host test).
#
# Usage:
#   scripts/smoke-test.sh [--release|--debug] [--scenario=NAME] [--timeout=SECS]
#
# Scenarios:
#   boot  — boot to the scheduler/shell cleanly (no script).
#   net   — boot + curltest.rc: DHCP, net_poll thread, HTTPS fetch exits 0.
#
# Requires a prior build (scripts/build.sh <type>) and a nix-shell providing
# qemu + OVMF + mtools (the same environment run-qemu.sh expects).

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "${SCRIPT_DIR}")"

BUILD_TYPE="release"
SCENARIO="net"
TIMEOUT=150

for arg in "$@"; do
    case "$arg" in
        --release)    BUILD_TYPE="release" ;;
        --debug)      BUILD_TYPE="debug" ;;
        --scenario=*) SCENARIO="${arg#--scenario=}" ;;
        --timeout=*)  TIMEOUT="${arg#--timeout=}" ;;
        *) echo "Unknown arg: $arg" >&2; exit 2 ;;
    esac
done

# --- Scenario definition: required (all) + forbidden (any) serial markers ----
QEMU_SCRIPT_ARG=""
case "${SCENARIO}" in
    boot)
        REQUIRED=("BOOT: complete")
        ;;
    net)
        QEMU_SCRIPT_ARG="--script=curltest"
        REQUIRED=(
            "net: DHCP ACK"
            "net: net_poll thread started"
            "sys_exit_group: 'curl"   # curl process exited...
        )
        # The curl exit line must also report status 0 (checked specially below).
        ;;
    *)
        echo "Unknown scenario: ${SCENARIO} (valid: boot, net)" >&2
        exit 2
        ;;
esac

FORBIDDEN=(
    "=== KERNEL FAULT ==="
    "*** KERNEL PANIC ***"
    "KERNEL PANIC:"
    "TRIPLE FAULT"
    "ASSERTION FAILED"
)

BUILD_DIR="${ROOT_DIR}/build/${BUILD_TYPE}"
if [ ! -d "${BUILD_DIR}" ]; then
    echo "FAIL: ${BUILD_DIR} not found — run scripts/build.sh ${BUILD_TYPE} first" >&2
    exit 1
fi

SERIAL_LOG="$(mktemp /tmp/brook-smoke-XXXXXX.log)"
MONITOR_SOCK="/tmp/qemu_monitor.sock"
QEMU_PID=""

cleanup() {
    # Ask QEMU to quit cleanly via its monitor; fall back to killing the tree.
    if [ -S "${MONITOR_SOCK}" ] && command -v socat >/dev/null 2>&1; then
        echo "quit" | socat - "UNIX-CONNECT:${MONITOR_SOCK}" 2>/dev/null || true
        sleep 1
    fi
    if [ -n "${QEMU_PID}" ] && kill -0 "${QEMU_PID}" 2>/dev/null; then
        # Kill the run-qemu.sh process group (negative PID) to take qemu with it.
        kill -TERM "-${QEMU_PID}" 2>/dev/null || kill -TERM "${QEMU_PID}" 2>/dev/null || true
        sleep 1
        kill -KILL "-${QEMU_PID}" 2>/dev/null || true
    fi
    rm -f "${SERIAL_LOG}"
}
trap cleanup EXIT

echo "==> Brook smoke gate"
echo "    scenario : ${SCENARIO}"
echo "    build    : ${BUILD_TYPE}"
echo "    timeout  : ${TIMEOUT}s"
echo "    serial   : ${SERIAL_LOG}"

# Launch in its own session/process-group so cleanup can reap the whole tree.
setsid "${SCRIPT_DIR}/run-qemu.sh" "--${BUILD_TYPE}" --headless ${QEMU_SCRIPT_ARG} \
    >"${SERIAL_LOG}" 2>&1 &
QEMU_PID=$!

# --- Poll the serial log -----------------------------------------------------
result=""
ELAPSED=0
while [ "${ELAPSED}" -lt "${TIMEOUT}" ]; do
    # Fail fast on any forbidden marker.
    for pat in "${FORBIDDEN[@]}"; do
        if grep -qF "${pat}" "${SERIAL_LOG}" 2>/dev/null; then
            result="fault: ${pat}"
            break 2
        fi
    done

    # Success when every required marker is present.
    all_present=1
    for pat in "${REQUIRED[@]}"; do
        grep -qF "${pat}" "${SERIAL_LOG}" 2>/dev/null || { all_present=0; break; }
    done
    if [ "${all_present}" -eq 1 ]; then
        # For the net scenario, the curl exit line must report status 0.
        if [ "${SCENARIO}" = "net" ]; then
            if grep -E "sys_exit_group: 'curl[^']*' .*status 0\b" "${SERIAL_LOG}" >/dev/null 2>&1; then
                result="pass"
            else
                # curl exited non-zero — keep waiting in case a retry succeeds,
                # but if its exit line is present with a non-zero status, fail.
                if grep -E "sys_exit_group: 'curl[^']*' .*status [1-9]" "${SERIAL_LOG}" >/dev/null 2>&1; then
                    result="fail: curl exited non-zero"
                    break
                fi
            fi
        else
            result="pass"
        fi
        [ "${result}" = "pass" ] && break
    fi

    # Detect early QEMU death (e.g. build/launch error).
    if ! kill -0 "${QEMU_PID}" 2>/dev/null; then
        # Give the log a moment to flush, then evaluate one last time.
        sleep 1
        if grep -qF "${REQUIRED[0]}" "${SERIAL_LOG}" 2>/dev/null; then
            : # fall through; markers may now be satisfied next loop
        else
            result="fail: qemu exited before reaching success markers"
            break
        fi
    fi

    sleep 2
    ELAPSED=$((ELAPSED + 2))
done

[ -z "${result}" ] && result="fail: timeout after ${TIMEOUT}s"

# --- Report ------------------------------------------------------------------
echo
case "${result}" in
    pass)
        echo "PASS: scenario '${SCENARIO}' completed cleanly."
        grep -E "net: DHCP ACK|net_poll thread started|sys_exit_group: 'curl|BOOT: complete" \
            "${SERIAL_LOG}" | sed 's/^/    /' || true
        exit 0
        ;;
    fault:*)
        echo "FAIL: ${result}"
        echo "---- serial context ----"
        grep -nF -A3 "${result#fault: }" "${SERIAL_LOG}" | head -20
        echo "---- last 20 lines ----"
        tail -20 "${SERIAL_LOG}"
        exit 1
        ;;
    *)
        echo "FAIL: ${result}"
        echo "---- last 30 lines of serial ----"
        tail -30 "${SERIAL_LOG}"
        exit 1
        ;;
esac
