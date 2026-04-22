#!/usr/bin/env bash
# Brings up a VDE switch that QEMU instances can peer on.
# No DHCP / NAT — guests use static IPs (see scripts/run-qemu-pair.sh).
#
# Sockets / pids:
#   /tmp/vde-brook.ctl     — switch control socket (passed to QEMU -netdev vde,sock=...)
#   /tmp/vde-brook.mgmt    — management socket
#   /tmp/vde-brook.switch.pid

set -euo pipefail

CTL=/tmp/vde-brook.ctl
MGMT=/tmp/vde-brook.mgmt
SWITCH_PID=/tmp/vde-brook.switch.pid

case "${1:-up}" in
    up)
        if [ -S "${CTL}" ] && [ -f "${SWITCH_PID}" ] \
               && kill -0 "$(cat "${SWITCH_PID}")" 2>/dev/null; then
            echo "VDE switch already running (pid $(cat ${SWITCH_PID}))"
            exit 0
        fi
        rm -f "${CTL}" "${MGMT}" "${SWITCH_PID}"
        echo "Starting vde_switch at ${CTL}..."
        vde_switch -sock "${CTL}" -mgmt "${MGMT}" -mgmtmode 0600 \
                   -daemon -pidfile "${SWITCH_PID}"
        echo "VDE up. Control socket: ${CTL}"
        echo "(Guests use static IPs via BROOK.CFG NET0_* keys; no DHCP on this switch.)"
        ;;
    down)
        if [ -f "${SWITCH_PID}" ]; then
            pid="$(cat "${SWITCH_PID}")"
            if kill -0 "${pid}" 2>/dev/null; then
                kill "${pid}" || true
            fi
            rm -f "${SWITCH_PID}"
        fi
        rm -f "${CTL}" "${MGMT}"
        echo "VDE down."
        ;;
    status)
        if [ -f "${SWITCH_PID}" ] && kill -0 "$(cat "${SWITCH_PID}")" 2>/dev/null; then
            echo "vde_switch: running (pid $(cat ${SWITCH_PID}))"
        else
            echo "vde_switch: stopped"
        fi
        ;;
    *)
        echo "Usage: $0 {up|down|status}" >&2
        exit 1
        ;;
esac
