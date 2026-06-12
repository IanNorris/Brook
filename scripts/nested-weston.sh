#!/usr/bin/env bash
# nested-weston.sh — manage a lock-proof nested headless weston for GPU screenshots.
#
# Why this exists: QEMU's `-display sdl,gl=on` renders into a Wayland surface,
# which the QEMU HMP monitor cannot screendump ("no surface"). To capture the
# accelerated (virgl/venus) display we run our OWN headless weston on the iGPU
# render node and point QEMU's SDL window at it; weston-screenshooter then writes
# a real PNG. Because the compositor is headless (no physical output) it is
# completely independent of the host login/lock state — screenshots keep working
# when the PC is locked.
#
# Two gotchas this script handles for you:
#   1. glvnd picks the nvidia EGL ICD first on this dual-GPU box, which makes
#      weston's surfaceless GL die ("EGL surfaceless platform cannot be used").
#      We force the Mesa-only EGL vendor dir (same trick run-qemu.sh uses).
#   2. weston 15 gates the screenshooter ("unauthorized") unless started with
#      --debug, which registers screenshot_allow_all. We always pass --debug.
#
# Usage:
#   scripts/nested-weston.sh start      # start headless weston (idempotent)
#   scripts/nested-weston.sh stop       # stop it
#   scripts/nested-weston.sh status     # is it running? print socket + env
#   scripts/nested-weston.sh shot FILE  # capture a PNG of the weston output
#   scripts/nested-weston.sh env        # print the env to point QEMU SDL at it
#
# After `start`, launch QEMU into it with:
#   eval "$(scripts/nested-weston.sh env)"
#   BROOK_GPU=gl BROOK_GPU_DISPLAY=sdl ./scripts/run-qemu.sh --release --headless --script glgears
# then `scripts/nested-weston.sh shot /tmp/out.png`.
set -euo pipefail

# --- Configuration (override via env) ---------------------------------------
WL_SOCKET="${BROOK_WL_SOCKET:-wayland-brook}"
WL_RUNTIME="${BROOK_WL_RUNTIME:-/tmp/brook-wl-rt}"
WL_W="${BROOK_WL_WIDTH:-1280}"
WL_H="${BROOK_WL_HEIGHT:-800}"
RENDERNODE="${BROOK_RENDERNODE:-/dev/dri/renderD128}"
LOG="${BROOK_WL_LOG:-/tmp/brook-weston.log}"

# --- Resolve weston + the Mesa GL stack -------------------------------------
# Reuse the same pre-resolved paths run-qemu.sh accepts, else look them up.
resolve_paths() {
    WESTON_PATH="${BROOK_GPU_WESTON:-$(nix build --no-link --print-out-paths nixpkgs#weston 2>/dev/null | tail -1)}"
    MESA_PATH="${BROOK_GPU_MESA:-$(nix build --no-link --print-out-paths nixpkgs#mesa 2>/dev/null | tail -1)}"
    GLVND_PATH="${BROOK_GPU_GLVND:-$(nix build --no-link --print-out-paths nixpkgs#libglvnd 2>/dev/null | tail -1)}"
    if [ -z "${WESTON_PATH}" ] || [ -z "${MESA_PATH}" ] || [ -z "${GLVND_PATH}" ]; then
        echo "ERROR: failed to resolve weston/mesa/libglvnd via nix." >&2
        exit 1
    fi
}

# GL env that forces Mesa (over the nvidia ICD) for surfaceless EGL.
gl_env() {
    echo "XDG_RUNTIME_DIR=${WL_RUNTIME}"
    echo "LIBGL_DRIVERS_PATH=${MESA_PATH}/lib/dri"
    echo "GBM_BACKENDS_PATH=${MESA_PATH}/lib/gbm"
    echo "__EGL_VENDOR_LIBRARY_DIRS=${MESA_PATH}/share/glvnd/egl_vendor.d"
    echo "LD_LIBRARY_PATH=${GLVND_PATH}/lib:${MESA_PATH}/lib"
}

is_running() {
    [ -S "${WL_RUNTIME}/${WL_SOCKET}" ] || return 1
    for p in /proc/[0-9]*/cmdline; do
        case "$(tr '\0' ' ' < "$p" 2>/dev/null)" in
            *"--socket=${WL_SOCKET}"*) return 0 ;;
        esac
    done
    return 1
}

weston_pid() {
    for p in /proc/[0-9]*/cmdline; do
        case "$(tr '\0' ' ' < "$p" 2>/dev/null)" in
            *"--socket=${WL_SOCKET}"*) basename "$(dirname "$p")"; return 0 ;;
        esac
    done
    return 1
}

cmd_start() {
    if is_running; then
        echo "nested weston already running (socket ${WL_RUNTIME}/${WL_SOCKET})"
        return 0
    fi
    resolve_paths
    if [ ! -e "${RENDERNODE}" ]; then
        echo "ERROR: no DRM render node at ${RENDERNODE}." >&2
        exit 1
    fi
    mkdir -p "${WL_RUNTIME}"; chmod 700 "${WL_RUNTIME}"
    echo "Starting nested headless weston on ${RENDERNODE} (socket ${WL_SOCKET}, ${WL_W}x${WL_H})..."
    # --debug authorizes weston-screenshooter (screenshot_allow_all).
    # --idle-time=0 stops weston blanking the output.
    setsid env \
        XDG_RUNTIME_DIR="${WL_RUNTIME}" \
        LIBGL_DRIVERS_PATH="${MESA_PATH}/lib/dri" \
        GBM_BACKENDS_PATH="${MESA_PATH}/lib/gbm" \
        __EGL_VENDOR_LIBRARY_DIRS="${MESA_PATH}/share/glvnd/egl_vendor.d" \
        LD_LIBRARY_PATH="${GLVND_PATH}/lib:${MESA_PATH}/lib" \
        "${WESTON_PATH}/bin/weston" \
            --backend=headless --renderer=gl \
            --width="${WL_W}" --height="${WL_H}" \
            --socket="${WL_SOCKET}" --idle-time=0 --debug \
        >"${LOG}" 2>&1 < /dev/null &
    # Wait for the socket (up to ~10s).
    for _ in $(seq 1 20); do
        [ -S "${WL_RUNTIME}/${WL_SOCKET}" ] && break
        sleep 0.5
    done
    if is_running; then
        echo "OK: weston up. Socket: ${WL_RUNTIME}/${WL_SOCKET}  Log: ${LOG}"
    else
        echo "ERROR: weston failed to start. Last log lines:" >&2
        tail -15 "${LOG}" >&2
        exit 1
    fi
}

cmd_stop() {
    if pid=$(weston_pid); then
        kill "$pid" 2>/dev/null || true
        sleep 1
        echo "stopped nested weston (pid $pid)"
    else
        echo "nested weston not running"
    fi
}

cmd_status() {
    if is_running; then
        echo "running: socket ${WL_RUNTIME}/${WL_SOCKET} (pid $(weston_pid))"
    else
        echo "not running"
        return 1
    fi
}

cmd_shot() {
    local out="${1:-/tmp/brook-weston-shot.png}"
    if ! is_running; then
        echo "ERROR: nested weston not running; run '$0 start' first." >&2
        exit 1
    fi
    resolve_paths
    # weston-screenshooter writes wayland-screenshot-<ts>.png in the CWD.
    local tmpdir; tmpdir="$(mktemp -d)"
    ( cd "$tmpdir" && env \
        XDG_RUNTIME_DIR="${WL_RUNTIME}" \
        WAYLAND_DISPLAY="${WL_SOCKET}" \
        "${WESTON_PATH}/bin/weston-screenshooter" )
    local png; png="$(ls -t "$tmpdir"/wayland-screenshot-*.png 2>/dev/null | head -1)"
    if [ -z "$png" ]; then
        echo "ERROR: no screenshot produced." >&2
        rm -rf "$tmpdir"
        exit 1
    fi
    mkdir -p "$(dirname "$out")"
    mv "$png" "$out"
    rm -rf "$tmpdir"
    echo "wrote $out"
}

cmd_env() {
    resolve_paths
    # Emit shell assignments to point QEMU's SDL window at nested weston.
    echo "export XDG_RUNTIME_DIR=${WL_RUNTIME}"
    echo "export WAYLAND_DISPLAY=${WL_SOCKET}"
    echo "export SDL_VIDEODRIVER=wayland"
}

case "${1:-}" in
    start)  cmd_start ;;
    stop)   cmd_stop ;;
    status) cmd_status ;;
    shot)   shift; cmd_shot "${1:-}" ;;
    env)    cmd_env ;;
    *)
        echo "usage: $0 {start|stop|status|shot FILE|env}" >&2
        exit 2
        ;;
esac
