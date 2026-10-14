#!/usr/bin/env bash
# Launch the locally prepared app checkpoint. Run inside nix-shell.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
checkpoint="${BROOK_APPS_RUN:-${ROOT_DIR}/build/experiments/results/apps-ready-20261014}"
for image in boot data nix; do
    if [ ! -f "${checkpoint}/${image}.img" ]; then
        echo "Missing prepared app checkpoint: ${checkpoint}/${image}.img" >&2
        echo "See docs/experiments.md for fresh guest installation instructions." >&2
        exit 1
    fi
done
export BROOK_EXPERIMENT_RUN="${checkpoint}"
export BROOK_EXPERIMENT_DISKS="${checkpoint}"
export BROOK_DISK_IMG="${BROOK_DISK_IMG:-${checkpoint}/boot.img}"
export BROOK_SMP="${BROOK_SMP:-1}"
export BROOK_VM_BRIDGE="${BROOK_VM_BRIDGE:-1}"
export BROOK_AUDIODEV="${BROOK_AUDIODEV:-none}"
export BROOK_EXPERIMENT_DISPLAY="${BROOK_EXPERIMENT_DISPLAY:-native}"
if [ "$BROOK_EXPERIMENT_DISPLAY" = native ]; then
    export BROOK_GPU_DISPLAY="${BROOK_GPU_DISPLAY:-sdl}"
fi
export BROOK_EXPERIMENT_TIMEOUT="${BROOK_EXPERIMENT_TIMEOUT:-28800}"
exec "${ROOT_DIR}/scripts/run_experiment.sh" desktop
