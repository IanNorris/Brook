#!/usr/bin/env bash
# Build the host LD_PRELOAD harness from Brook's kernel i915 sources.
# Run inside the pinned dev shell: nix-shell --run tools/vaapi-brook-pkg/host-shim/build.sh [OUT]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
K="$ROOT/src/kernel/src"
OUT="${1:-$ROOT/build/brook-i915-shim/libbrook_i915_shim.so}"
mkdir -p "$(dirname "$OUT")"
CXX="${HOST_CXX:-c++}"
FLAGS=(-O1 -g -fPIC -std=gnu++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fvisibility=default
       -DBROOK_HOST_TEST -DBROOK_SSD_AUTO_RECOVERY -iquote"$K" -iquote"$K/../../shared" -iquote"$K/../../shared/inc_km")
"$CXX" "${FLAGS[@]}" -shared -nostdlib++ -o "$OUT" \
  "$ROOT/tools/vaapi-brook-pkg/host-shim/brook_i915_shim.cpp" \
  "$K/intel_bdw_buffer.cpp" "$K/intel_bdw_file.cpp" "$K/intel_bdw_exec_snapshot.cpp" "$K/intel_bdw_ioctl.cpp" \
  -ldl -lpthread -Wl,--no-undefined
echo "$OUT"
