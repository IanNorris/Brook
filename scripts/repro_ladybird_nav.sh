#!/usr/bin/env bash
# Reproduce: Ladybird second-navigation deadlock.
#
# Boots Brook with Ladybird loading example.com, waits for page load,
# then clicks the address bar, types a second URL (wiki.osdev.org),
# and presses Enter. Captures serial log showing the IPC deadlock.
#
# Usage: scripts/repro_ladybird_nav.sh [--timeout SECS]
#        Default timeout: 180 seconds total.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
cd "${ROOT_DIR}"

TIMEOUT="${1:-180}"
SERIAL_LOG="$(mktemp /tmp/brook-repro-nav-XXXXXX.log)"
QMON_SOCK="$(mktemp -u /tmp/brook-repro-qmon-XXXXXX.sock)"
QMON_FIFO="$(mktemp -u /tmp/brook-repro-qmon-fifo-XXXXXX)"

cleanup() {
    [ -n "${MON_PID:-}" ] && kill "${MON_PID}" 2>/dev/null || true
    if [ -n "${QEMU_PID:-}" ]; then
        kill "${QEMU_PID}" 2>/dev/null || true
        wait "${QEMU_PID}" 2>/dev/null || true
    fi
    rm -f "${QMON_SOCK}" "${QMON_FIFO}"
}
trap cleanup EXIT

echo "=== Ladybird second-navigation deadlock repro ==="
echo "    serial log: ${SERIAL_LOG}"
echo "    timeout:    ${TIMEOUT}s total"

# Send a batch of QEMU monitor commands via a persistent Python connection.
# Each line: "CMD [delay_secs]"
# e.g.: send_monitor_commands "mouse_move 179 118" "0.2" "mouse_button 1" "0.1"
send_monitor_batch() {
    python3 -u -c "
import socket, sys, time, os

sock_path = '${QMON_SOCK}'
cmds = sys.argv[1:]

# Wait for socket
for _ in range(60):
    if os.path.exists(sock_path):
        break
    time.sleep(0.5)
else:
    print('ERROR: monitor socket never appeared', file=sys.stderr)
    sys.exit(1)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(sock_path)
# Drain banner
time.sleep(0.3)
try: s.recv(8192)
except: pass

# Process command/delay pairs
i = 0
while i < len(cmds):
    cmd = cmds[i]
    delay = float(cmds[i+1]) if i+1 < len(cmds) else 0.15
    s.sendall((cmd + '\n').encode())
    time.sleep(delay)
    try: s.recv(4096)
    except: pass
    i += 2

s.close()
" "$@"
}

# Wait for a pattern to appear in the serial log
wait_for() {
    local pattern="$1" desc="$2" max_wait="${3:-90}"
    echo -n "    waiting for ${desc}..."
    for i in $(seq 1 "${max_wait}"); do
        if grep -q "${pattern}" "${SERIAL_LOG}" 2>/dev/null; then
            echo " ok (${i}s)"
            return 0
        fi
        sleep 1
    done
    echo " TIMEOUT after ${max_wait}s"
    return 1
}

# --- Boot QEMU ---
export BROOK_SKIP_UPDATE_DISK=1
export SERIAL_OPT="-serial file:${SERIAL_LOG} -monitor unix:${QMON_SOCK},server,nowait"

setsid timeout "${TIMEOUT}" bash scripts/run-qemu.sh --release \
    --headless --no-audio --script=wayland_ladybird_example \
    >/dev/null 2>&1 &
QEMU_PID=$!

# --- Wait for first page to load ---
if ! wait_for "Example Domain - Ladybird" "first page load" 120; then
    echo "FAIL: first page never loaded"
    echo "--- serial tail ---"
    tail -40 "${SERIAL_LOG}" 2>/dev/null || true
    exit 1
fi

# Count initial DNS queries so we can detect new ones from navigation
INITIAL_DNS=$(grep -c "dns_response" "${SERIAL_LOG}" 2>/dev/null || echo 0)
echo "    initial DNS count: ${INITIAL_DNS}"

# Extra settle time for rendering
sleep 3

# --- Click address bar, select all, type URL, press Enter ---
# All in one persistent monitor connection to avoid dropped commands.
#
# Coordinate mapping (verified empirically):
#   Screen resolution: 1920x1080
#   Ladybird window: created at (40,40) 800x600 with server-side deco
#   SSD titlebar height: 26px (measured from test run)
#   Client area top-left on screen: (40, 40+26) = (40, 66)
#   Ian's address bar click: surface-relative (139, 52)
#   Screen absolute: (40+139, 66+52) = (179, 118)
echo "    clicking address bar, typing URL, pressing Enter..."
send_monitor_batch \
    "mouse_move 179 118"   "0.3" \
    "mouse_button 1"       "0.1" \
    "mouse_button 0"       "0.5" \
    "mouse_button 1"       "0.1" \
    "mouse_button 0"       "0.5" \
    "sendkey ctrl-a"       "0.5" \
    "sendkey w"            "0.1" \
    "sendkey i"            "0.1" \
    "sendkey k"            "0.1" \
    "sendkey i"            "0.1" \
    "sendkey dot"          "0.1" \
    "sendkey o"            "0.1" \
    "sendkey s"            "0.1" \
    "sendkey d"            "0.1" \
    "sendkey e"            "0.1" \
    "sendkey v"            "0.1" \
    "sendkey dot"          "0.1" \
    "sendkey o"            "0.1" \
    "sendkey r"            "0.1" \
    "sendkey g"            "0.3" \
    "sendkey ret"          "0.5"

NAV_TIME=$(date +%s)
echo "    navigation initiated at $(date)"

# --- Wait and observe ---
# The bug: after DNS resolves for wiki.osdev.org, RequestServer's IPC
# message to the main process gets no response. We watch for either:
#   1. Success: "wiki.osdev.org" appears in set_title (page loaded!)
#   2. Deadlock: new DNS queries appear but no TCP connection follows
echo "    monitoring for deadlock..."

RESULT="unknown"
for i in $(seq 1 90); do
    # Check for success: title contains the new URL
    if grep -qE "(wiki\.osdev|osdev\.org).*Ladybird|Ladybird.*(wiki\.osdev|osdev\.org)" "${SERIAL_LOG}" 2>/dev/null; then
        RESULT="success"
        break
    fi

    # Check for deadlock: new DNS queries appeared (beyond initial page load)
    CUR_DNS=$(grep -c "dns_response" "${SERIAL_LOG}" 2>/dev/null || echo 0)
    NEW_DNS=$((CUR_DNS - INITIAL_DNS))
    if [ "${NEW_DNS}" -ge 1 ]; then
        # DNS for the second URL resolved. Wait 15s to see if a TCP
        # connection follows. If not, it's the deadlock.
        echo "    new DNS detected (${NEW_DNS} new queries), waiting 15s for TCP..."
        sleep 15
        if grep -qE "(wiki\.osdev|osdev\.org).*Ladybird|Ladybird.*(wiki\.osdev|osdev\.org)" "${SERIAL_LOG}" 2>/dev/null; then
            RESULT="success"
        else
            # Check for the specific deadlock signature: RS poll with no
            # activity after DNS
            RESULT="deadlock"
        fi
        break
    fi

    sleep 1
done

echo ""
echo "=== RESULT: ${RESULT} ==="
echo ""

if [ "${RESULT}" = "deadlock" ]; then
    echo "Deadlock reproduced! Key diagnostics from serial log:"
    echo ""
    echo "--- DNS queries ---"
    grep "dns_query\|dns_response" "${SERIAL_LOG}" | tail -10
    echo ""
    echo "--- Unix socket write waiter state ---"
    grep "unix_write" "${SERIAL_LOG}" | tail -10
    echo ""
    echo "--- Pipe wake events ---"
    grep "pipe_wake_RS\|pipe_write_RS" "${SERIAL_LOG}" | tail -10
    echo ""
    echo "--- RequestServer poll (last 5) ---"
    grep "poll_enter.*timeout=-1" "${SERIAL_LOG}" | tail -10
    echo ""
    echo "--- STRACE_RS (last 30) ---"
    grep "STRACE" "${SERIAL_LOG}" | tail -30
    echo ""
    echo "Full log: ${SERIAL_LOG}"
    exit 0  # Deadlock reproduced = success for repro script
elif [ "${RESULT}" = "success" ]; then
    echo "Page loaded successfully — bug did NOT reproduce this run."
    echo "Full log: ${SERIAL_LOG}"
    exit 1  # Bug didn't reproduce = failure for repro script
else
    echo "Inconclusive — neither success nor clear deadlock detected."
    echo ""
    echo "--- waylandd events ---"
    grep "waylandd" "${SERIAL_LOG}" | tail -20
    echo ""
    echo "--- DNS ---"
    grep "dns_" "${SERIAL_LOG}" | tail -10
    echo ""
    echo "--- serial tail ---"
    tail -40 "${SERIAL_LOG}" 2>/dev/null || true
    echo "Full log: ${SERIAL_LOG}"
    exit 2
fi
