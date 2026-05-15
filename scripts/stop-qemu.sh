#!/usr/bin/env bash
# Stop a running Brook QEMU instance via its monitor socket.
# Falls back to SIGTERM if the socket isn't available.
set -euo pipefail

MONITOR_SOCK="${BROOK_MONITOR_SOCK:-/tmp/brook-qemu-monitor.sock}"

if [ -S "${MONITOR_SOCK}" ]; then
    echo "quit" | socat - UNIX-CONNECT:"${MONITOR_SOCK}" 2>/dev/null && exit 0
fi

# Fallback: find QEMU process by its serial log argument
for f in /proc/[0-9]*/cmdline; do
    if grep -q "brook" "$f" 2>/dev/null && grep -q "qemu" "$f" 2>/dev/null; then
        PID=$(echo "$f" | cut -d/ -f3)
        if [ -n "$PID" ] && [ "$PID" != "self" ]; then
            kill "$PID" 2>/dev/null && echo "Stopped QEMU (PID $PID)" && exit 0
        fi
    fi
done

echo "No running Brook QEMU instance found"
exit 1
