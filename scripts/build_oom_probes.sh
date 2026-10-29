#!/usr/bin/env bash
# Bounded native recovery probes; disposable private guest images only.
set -euo pipefail
root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root_dir"
: "${MUSL_CC:?Run this script inside nix-shell}"
output_dir="${1:-build/oom-probes}"
mkdir -p "$output_dir"
for source in scripts/workloads/oom_*.c; do
    name="$(basename "$source" .c)"
    "$MUSL_CC" -O2 -static -Wall -Wextra -Werror "$source" -o "$output_dir/$name"
done
