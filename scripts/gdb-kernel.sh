#!/usr/bin/env bash
# Attach GDB to a running QEMU kernel debug session.
# Run with: ./scripts/run-qemu.sh --debug  (in one terminal)
# Then:     ./scripts/gdb-kernel.sh        (in another terminal)
#
# Inside GDB:
#   (gdb) continue      -- let the kernel run
#   (gdb) Ctrl-C        -- pause and inspect
#   (gdb) bt            -- backtrace
#   (gdb) info reg      -- CPU registers
#   (gdb) x/20i $rip    -- disassemble at current PC

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
ELF="${ROOT_DIR}/build/kernel/BROOK.elf"

if [ ! -f "$ELF" ]; then
    echo "ERROR: $ELF not found — run scripts/build.sh first"
    exit 1
fi

nix-shell --run "
gdb -q \
    -ex 'set architecture i386:x86-64' \
    -ex 'target remote localhost:1234' \
    -ex 'symbol-file ${ELF}' \
    -ex 'set confirm off' \
    \$@
"
