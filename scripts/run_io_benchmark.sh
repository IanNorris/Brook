#!/usr/bin/env bash
# Run inside nix-shell; output is a new directory below build/.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
: "${MUSL_CC:?Run this script inside nix-shell}"
mkdir -p build/io-bench
"$MUSL_CC" -O2 -Wall -Wextra -static -pthread tools/io-bench/bench.c -o build/io-bench/io-bench
"$MUSL_CC" -O2 -Wall -Wextra -static tools/io-bench/cachecheck.c -o build/io-bench/cachecheck
"$MUSL_CC" -O2 -Wall -Wextra -static tools/io-bench/mixedappendcheck.c -o build/io-bench/mixedappendcheck
mkdir -p build/package-cache
"$MUSL_CC" -O2 -Wall -Wextra -static tools/at-syscall-probe/filelifecheck.c -o build/package-cache/filelifecheck
for probe in waitmaskcheck childsignalcheck selectcheck; do
    "$MUSL_CC" -O2 -Wall -Wextra -static "tools/at-syscall-probe/${probe}.c" -o "build/io-bench/${probe}"
done
"$MUSL_CC" -O2 -Wall -Wextra -static tools/at-syscall-probe/flushcheck.c -o build/io-bench/flushcheck
"$MUSL_CC" -O2 -Wall -Wextra -static -pthread tools/at-syscall-probe/threadpagecheck.c -o build/io-bench/threadpagecheck
"$MUSL_CC" -O2 -Wall -Wextra -static -pthread tools/at-syscall-probe/exitgroupcheck.c -o build/io-bench/exitgroupcheck
"$MUSL_CC" -O2 -Wall -Wextra -static -pthread tools/at-syscall-probe/fatalgroupcheck.c -o build/io-bench/fatalgroupcheck
python3 scripts/io_acceptance.py "$@"
