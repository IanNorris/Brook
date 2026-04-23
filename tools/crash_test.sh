#!/usr/bin/env bash
# crash_test.sh — Automated crash test & decode pipeline for Brook OS
#
# Usage:
#   ./tools/crash_test.sh [mode]     # Test a specific crash mode
#   ./tools/crash_test.sh --all      # Test all crash modes
#   ./tools/crash_test.sh --list     # List available modes
#
# Modes: panic, nullptr, divzero, gpf, ud, stackoverflow
#
# Requires: QEMU, Python3 with Pillow + pyzbar, built kernel

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
VNC_PORT=5943
VNC_DISPLAY=":43"
TIMEOUT=20
CAPTURE_PATH="/tmp/brook_crash_capture.png"

MODES=(panic nullptr divzero gpf ud)

RED='\033[91m'
GREEN='\033[92m'
YELLOW='\033[93m'
CYAN='\033[96m'
RESET='\033[0m'

log()  { echo -e "  ${CYAN}▸${RESET} $*"; }
pass() { echo -e "  ${GREEN}✓${RESET} $*"; }
fail() { echo -e "  ${RED}✗${RESET} $*"; }
warn() { echo -e "  ${YELLOW}⚠${RESET} $*"; }

usage() {
    echo "Usage: $0 [mode|--all|--list]"
    echo ""
    echo "Modes: ${MODES[*]}"
    echo "  --all    Run all crash modes"
    echo "  --list   List available modes"
    exit 1
}

# Create a temporary boot script for the given crash mode
create_boot_script() {
    local mode="$1"
    local script_path="/tmp/brook_crashtest_${mode}.rc"
    cat > "$script_path" <<EOF
# Brook OS crash test — ${mode}
set tty full
crashtest ${mode}
EOF
    echo "$script_path"
}

# Run QEMU with a boot script, wait for panic, capture VNC, decode
run_crash_test() {
    local mode="$1"
    log "Testing crash mode: ${YELLOW}${mode}${RESET}"

    # Create boot script
    local boot_script
    boot_script=$(create_boot_script "$mode")

    # Copy boot script to data/scripts so run-qemu.sh can find it
    local script_name="crashtest_${mode}"
    cp "$boot_script" "${ROOT_DIR}/data/scripts/${script_name}.rc"

    # Start QEMU in background
    local qemu_log="/tmp/brook_qemu_${mode}.log"
    bash "${ROOT_DIR}/scripts/run-qemu.sh" --headless --vnc "${VNC_DISPLAY}" \
        --script="${script_name}" &>"$qemu_log" &
    local qemu_pid=$!

    # Wait for the OS to boot and panic
    log "  Waiting ${TIMEOUT}s for panic..."
    sleep "$TIMEOUT"

    # Check if QEMU is still running (it should be — panic uses pause loop)
    if ! kill -0 "$qemu_pid" 2>/dev/null; then
        fail "QEMU exited prematurely for mode '${mode}'"
        tail -5 "$qemu_log" 2>/dev/null || true
        return 1
    fi

    # Capture VNC and decode
    log "  Capturing VNC screenshot..."
    local decode_output
    decode_output=$(python3 "${ROOT_DIR}/tools/crash_decoder.py" \
        --vnc "localhost:${VNC_PORT}" --no-color 2>&1) || {
        fail "Decoder failed for mode '${mode}'"
        echo "$decode_output"
        kill "$qemu_pid" 2>/dev/null || true
        wait "$qemu_pid" 2>/dev/null || true
        return 1
    }

    # Kill QEMU
    kill "$qemu_pid" 2>/dev/null || true
    wait "$qemu_pid" 2>/dev/null || true

    # Validate output contains expected fields
    local ok=1
    if echo "$decode_output" | grep -q "BROOK OS CRASH DUMP"; then
        pass "  Crash dump header present"
    else
        fail "  Missing crash dump header"
        ok=0
    fi

    if echo "$decode_output" | grep -q "RIP.*0x"; then
        pass "  RIP register captured"
    else
        fail "  Missing RIP register"
        ok=0
    fi

    if echo "$decode_output" | grep -q "Stack Trace"; then
        pass "  Stack trace present"
    else
        fail "  Missing stack trace"
        ok=0
    fi

    if echo "$decode_output" | grep -q "+0x"; then
        pass "  Symbols resolved"
    else
        warn "  No symbols resolved (may be OK for some modes)"
    fi

    # Clean up temp script
    rm -f "${ROOT_DIR}/data/scripts/${script_name}.rc"
    rm -f "$boot_script"

    if [ "$ok" -eq 1 ]; then
        pass "Mode '${mode}' — ${GREEN}PASSED${RESET}"
        return 0
    else
        fail "Mode '${mode}' — ${RED}FAILED${RESET}"
        echo "$decode_output"
        return 1
    fi
}

# Main
if [ $# -eq 0 ]; then
    usage
fi

case "$1" in
    --list)
        echo "Available crash test modes:"
        for m in "${MODES[@]}"; do
            echo "  $m"
        done
        ;;
    --all)
        echo -e "\n${CYAN}═══ Brook OS Crash Test Suite ═══${RESET}\n"
        passed=0
        failed=0
        for m in "${MODES[@]}"; do
            if run_crash_test "$m"; then
                ((passed++))
            else
                ((failed++))
            fi
            echo ""
        done
        echo -e "${CYAN}═══ Results: ${GREEN}${passed} passed${RESET}, ${RED}${failed} failed${RESET} ═══\n"
        [ "$failed" -eq 0 ]
        ;;
    *)
        # Single mode
        found=0
        for m in "${MODES[@]}"; do
            if [ "$1" = "$m" ]; then
                found=1
                break
            fi
        done
        if [ "$found" -eq 0 ]; then
            echo "Unknown mode: $1"
            echo "Available: ${MODES[*]}"
            exit 1
        fi
        run_crash_test "$1"
        ;;
esac
