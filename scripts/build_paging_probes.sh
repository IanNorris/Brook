#!/usr/bin/env bash
# Rebuild native paging acceptance probes from the current checkout, not cached
# binaries from a previous tier/ABI experiment. Run inside nix-shell.
set -euo pipefail
root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root_dir"
: "${MUSL_CC:?Run this script inside nix-shell}"
output_dir="${1:-build/paging-probes}"
mkdir -p "$output_dir"
for source in scripts/workloads/pageable_*.c; do
    name="$(basename "$source" .c)"
    "$MUSL_CC" -O2 -static -Wall -Wextra -Werror "$source" -o "$output_dir/$name"
done
