#!/usr/bin/env bash
#
# host_build_and_run.sh — one-shot "build Brook and launch it in a window" for a
# real host with a display + GPU. Wraps the env-var dance so you don't have to
# remember it. Keep this script up to date as the run flags evolve.
#
# Quick start (from the repo root):
#   ./scripts/host_build_and_run.sh                  # build Release + GPU desktop (wm)
#   ./scripts/host_build_and_run.sh gltri            # build + run the GL triangle
#   ./scripts/host_build_and_run.sh --no-build wm    # skip the build, just run
#   ./scripts/host_build_and_run.sh --cpu wm         # CPU compositor (no virgl)
#   ./scripts/host_build_and_run.sh --headless glprobe   # no window (serial only)
#
# What it does:
#   1. (unless --no-build) builds the kernel + apps + updates the disk image
#      via build_all.sh, which already runs build_apps.sh and update_disk.sh.
#   2. launches run-qemu.sh with the GPU-composited desktop in a real SDL
#      window (hardware virgl on the host GPU), running the chosen boot script.
#
# Notes:
#   - The accelerated SDL window needs your X11/Wayland session reachable by the
#     qemu_full binary, and a DRM render node (default /dev/dri/renderD128).
#   - run-qemu.sh resolves qemu_full + mesa + libglvnd from nixpkgs the first
#     time; subsequent runs reuse the nix store paths.
#   - This is for INTERACTIVE host use. CI/headless validation uses run-qemu.sh
#     --headless directly (see the per-bug repro recipes).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$ROOT_DIR"

# --- Defaults (override via flags / env) ------------------------------------
BUILD=1                       # --no-build to skip
BUILD_TYPE="Release"          # --debug for a Debug build
COMPOSITE="gpu"               # --cpu for the CPU compositor
GPU_MODE="${BROOK_GPU:-gl}"   # gl (virgl) | venus | 0 (off); --venus / --no-gpu
GPU_DISPLAY="sdl"             # windowed accelerated present; --headless for none
SCRIPT_NAME="wm"              # boot script (last positional arg overrides)
EXTRA=()

usage() {
    sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    echo
    echo "Common boot scripts: wm, desktop, glassdemo, gltri, wm8doom,"
    echo "                     drmprobe, drmclear, glprobe, xmmtest"
    exit 0
}

for arg in "$@"; do
    case "$arg" in
        -h|--help)     usage ;;
        --no-build)    BUILD=0 ;;
        --debug)       BUILD_TYPE="Debug" ;;
        --release)     BUILD_TYPE="Release" ;;
        --cpu)         COMPOSITE="cpu" ;;
        --gpu)         COMPOSITE="gpu" ;;
        --venus)       GPU_MODE="venus" ;;
        --no-gpu)      GPU_MODE="0" ;;
        --headless)    GPU_DISPLAY="headless"; EXTRA+=("--headless") ;;
        --gtk)         GPU_DISPLAY="gtk" ;;   # NB: this qemu_full's gtk lacks GL
        --*)           EXTRA+=("$arg") ;;     # pass through unknown flags
        *)             SCRIPT_NAME="$arg" ;;  # positional = boot script name
    esac
done

# --- Build ------------------------------------------------------------------
if [ "$BUILD" -eq 1 ]; then
    echo "=== host_build_and_run: building ${BUILD_TYPE} (kernel + apps + disk) ==="
    nix-shell --run "./scripts/build_all.sh ${BUILD_TYPE}"
else
    echo "=== host_build_and_run: skipping build (--no-build) ==="
fi

# --- Compose the run environment --------------------------------------------
# run-qemu.sh wants the build type as --release/--debug.
BUILD_FLAG="--release"
[ "$BUILD_TYPE" = "Debug" ] && BUILD_FLAG="--debug"

RUN_ENV=()
if [ "$GPU_MODE" != "0" ]; then
    RUN_ENV+=("BROOK_GPU=${GPU_MODE}" "BROOK_GPU_DISPLAY=${GPU_DISPLAY}")
fi
RUN_ENV+=("BROOK_COMPOSITE=${COMPOSITE}")

echo "=== host_build_and_run: launching script='${SCRIPT_NAME}'"
echo "    gpu=${GPU_MODE} display=${GPU_DISPLAY} composite=${COMPOSITE} build=${BUILD_TYPE}"
echo "    env: ${RUN_ENV[*]}"

# --- Run --------------------------------------------------------------------
exec env "${RUN_ENV[@]}" \
    nix-shell --run "./scripts/run-qemu.sh ${BUILD_FLAG} --script ${SCRIPT_NAME} ${EXTRA[*]}"
