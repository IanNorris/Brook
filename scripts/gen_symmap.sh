#!/usr/bin/env bash
# Generate a symbol map from the Brook kernel ELF for use with the profiler.
# Usage: scripts/gen_symmap.sh [build_type]   # default: release
set -euo pipefail

BUILD_TYPE="${1:-release}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
ELF="${ROOT_DIR}/build/${BUILD_TYPE}/kernel/BROOK.elf"
OUT="${ROOT_DIR}/build/${BUILD_TYPE}/brook_symbols.txt"

if [ ! -f "$ELF" ]; then
    echo "Kernel ELF not found: $ELF"
    exit 1
fi

# Use llvm-nm if available, otherwise nm
NM=""
for candidate in llvm-nm nm; do
    if command -v "$candidate" &>/dev/null; then
        NM="$candidate"
        break
    fi
done

if [ -z "$NM" ]; then
    echo "No nm/llvm-nm found. Install llvmPackages or binutils."
    exit 1
fi

"$NM" -n --demangle "$ELF" 2>/dev/null | \
    grep -E '^[0-9a-f]+ [TtWw]' | \
    awk '{addr=$1; $1=""; $2=""; gsub(/^ +/, ""); print addr, $0}' > "$OUT"

COUNT=$(wc -l < "$OUT")
echo "Generated $OUT ($COUNT symbols)"
echo "Usage: python3 scripts/profiler_to_speedscope.py serial.log output.json --symmap $OUT"
