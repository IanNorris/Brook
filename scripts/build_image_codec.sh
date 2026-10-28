#!/usr/bin/env bash
# Build the repository-pinned codec for Python's --lz4-library option.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
compiler="${HOST_CC:-cc}"
link_flag=-shared
suffix=so
if [[ "$(uname -s)" == Darwin ]]; then link_flag=-dynamiclib; suffix=dylib; fi
output="${1:-${root}/build/host-tools/libbrook-image-lz4.${suffix}}"
mkdir -p "$(dirname "$output")"
"$compiler" -O3 -fPIC "$link_flag" "${root}/src/third_party/lz4/lz4.c" -o "$output"
printf '%s\n' "$output"
