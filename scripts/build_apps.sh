#!/usr/bin/env bash
# Build userspace test applications for Brook OS.
# Produces static musl-linked ELF64 binaries.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
SRC_DIR="${ROOT_DIR}/src/apps"
OUT_DIR="${ROOT_DIR}/build/apps"

CC="${MUSL_CC:-x86_64-unknown-linux-musl-gcc}"
CFLAGS="-static -O2 -Wall -Wextra"

mkdir -p "$OUT_DIR"

for src in "$SRC_DIR"/*.c; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c)
    echo "  APP  $name"
    $CC $CFLAGS -o "$OUT_DIR/$name" "$src"
done

echo "Done: $(ls "$OUT_DIR" | wc -l) app(s) in $OUT_DIR/"
