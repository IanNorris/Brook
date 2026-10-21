#!/usr/bin/env bash
# Start an already prepared private review; preparation and builds are separate.
set -euo pipefail
if [ "$#" -ne 1 ]; then
    echo "Usage: $0 REVIEW_DIRECTORY" >&2
    exit 2
fi
review_dir="$(cd -- "$1" && pwd)"
if [ ! -x "$review_dir/launch.sh" ]; then
    echo "Missing executable review launcher: $review_dir/launch.sh" >&2
    exit 1
fi
# Nix needs TMPDIR before entering its shell. run_experiment.sh runs too late
# to create it, and a freshly copied review need not contain this directory.
mkdir -p -- "$review_dir/tmp-host"
export TMPDIR="$review_dir/tmp-host"
# Apply affinity before QEMU creates vCPU, audio and renderer threads. Changing
# only QEMU's process leader later leaves its existing threads unrestricted.
if [ -n "${BROOK_REVIEW_CPUSET:-}" ]; then
    exec taskset --cpu-list "$BROOK_REVIEW_CPUSET" "$review_dir/launch.sh"
fi
exec "$review_dir/launch.sh"
