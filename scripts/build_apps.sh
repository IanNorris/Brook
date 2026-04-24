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
COREMARK_DIR="${ROOT_DIR}/../coremark"

# libcrashdump: user-mode crash-dump writer, auto-linked into every app.
# Rebuild if source or header changed.
CRASHDUMP_SRC="${ROOT_DIR}/src/shared/src_um/crash_dump.c"
CRASHDUMP_OBJ="${OUT_DIR}/crash_dump.o"
mkdir -p "$OUT_DIR"
if [ -f "$CRASHDUMP_SRC" ]; then
    if [ ! -f "$CRASHDUMP_OBJ" ] || [ "$CRASHDUMP_SRC" -nt "$CRASHDUMP_OBJ" ] || \
       [ "${ROOT_DIR}/src/shared/inc_um/crash_dump.h" -nt "$CRASHDUMP_OBJ" ]; then
        echo "  LIB  crash_dump"
        $CC $CFLAGS -c -I"${ROOT_DIR}/src/shared/inc_um" \
            -o "$CRASHDUMP_OBJ" "$CRASHDUMP_SRC"
    fi
    EXTRA_OBJS="$CRASHDUMP_OBJ"
else
    EXTRA_OBJS=""
fi

mkdir -p "$OUT_DIR"

# Build simple single-file apps
for src in "$SRC_DIR"/*.c; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c)
    [ "$name" = "coremark_wrapper" ] && continue  # skip placeholder
    echo "  APP  $name"
    $CC $CFLAGS -o "$OUT_DIR/$name" "$src" $EXTRA_OBJS
done

# Build CoreMark if source is available
if [ -d "$COREMARK_DIR" ]; then
    echo "  APP  coremark"
    $CC $CFLAGS \
        -DPERFORMANCE_RUN=1 -DITERATIONS=10000 \
        -DFLAGS_STR=\""-O2 -static"\" \
        -I"$COREMARK_DIR" -I"$COREMARK_DIR/posix" \
        "$COREMARK_DIR"/core_main.c \
        "$COREMARK_DIR"/core_list_join.c \
        "$COREMARK_DIR"/core_matrix.c \
        "$COREMARK_DIR"/core_state.c \
        "$COREMARK_DIR"/core_util.c \
        "$COREMARK_DIR"/posix/core_portme.c \
        -o "$OUT_DIR/coremark"
fi

echo "Done: $(ls "$OUT_DIR" | wc -l) app(s) in $OUT_DIR/"
