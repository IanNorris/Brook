#!/usr/bin/env bash
# Run a scenario against the new experiment disks. Use inside nix-shell.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "${1:-desktop}" in
    desktop) scenario=experiment_desktop ;;
    video) scenario=experiment_video ;;
    quake-soft) scenario=experiment_quake_soft ;;
    quake-gl3)
        scenario=experiment_quake_gl3
        export BROOK_GPU="${BROOK_GPU:-gl}"
        export BROOK_COMPOSITE="${BROOK_COMPOSITE:-gpu}"
        ;;
    office) scenario=experiment_office ;;
    bootstrap) scenario=experiment_bootstrap ;;
    *) echo "Usage: $0 [desktop|video|quake-soft|quake-gl3|office|bootstrap] [QEMU arguments...]" >&2; exit 2 ;;
esac
disks="${BROOK_EXPERIMENT_DISKS:-${ROOT_DIR}/build/experiments/seeds}"
source_boot="${BROOK_DISK_IMG:-${ROOT_DIR}/brook_disk.img}"
for image in "${source_boot}" "${disks}/data.img" "${disks}/nix.img"; do
    if [ ! -f "${image}" ]; then
        echo "Missing ${image}; run scripts/create_experiment_disks.py first." >&2
        exit 1
    fi
done
results="${ROOT_DIR}/build/experiments/results"
mkdir -p "${results}"
if [ -n "${BROOK_EXPERIMENT_RUN:-}" ]; then
    run_dir="${BROOK_EXPERIMENT_RUN}"
    mkdir -p "${run_dir}"
else
    run_dir=$(mktemp -d "${results}/${scenario}-XXXXXX")
fi
for name in data nix; do
    if [ ! -f "${run_dir}/${name}.img" ]; then
        # Each experiment gets writable clones; the verified seeds stay intact.
        cp --reflink=auto --sparse=always "${disks}/${name}.img" "${run_dir}/${name}.img"
    fi
done
export BROOK_EXT2_DISK="${run_dir}/data.img"
export BROOK_NIX_DISK="${run_dir}/nix.img"
if [ ! -f "${run_dir}/boot.img" ]; then
    cp --reflink=auto --sparse=always "${source_boot}" "${run_dir}/boot.img"
fi
export BROOK_DISK_IMG="${run_dir}/boot.img"
export BROOK_VM_CLONED_DISKS=1
export BROOK_SKIP_UPDATE_DISK=1
export BROOK_SMP="${BROOK_SMP:-8}"
# PS/2 is the default keyboard, matching 7b0e50a. Preserve explicit caller
# opt-in for USB experiments rather than overriding the launch default.
export SERIAL_OPT="-serial file:${run_dir}/serial.log"
instance="${BROOK_INSTANCE:-98}"
display_args=(--headless --vnc "127.0.0.1:${instance}")
case "${BROOK_EXPERIMENT_DISPLAY:-headless}" in
    headless) ;;
    native) display_args=() ;;
    *) echo "BROOK_EXPERIMENT_DISPLAY must be headless or native" >&2; exit 2 ;;
esac
echo "Run: ${run_dir}"
echo "Serial: ${run_dir}/serial.log"
if [ "${BROOK_EXPERIMENT_DISPLAY:-headless}" = headless ]; then
    echo "VNC: 127.0.0.1:$((5900 + instance)); monitor instance: ${instance}"
else
    echo "Native QEMU window; monitor instance: ${instance}"
fi
exec timeout "${BROOK_EXPERIMENT_TIMEOUT:-600}" "${ROOT_DIR}/scripts/run-qemu.sh" \
    --release "${display_args[@]}" --instance="${instance}" \
    --real-esp --script "${scenario}" "${@:2}"
