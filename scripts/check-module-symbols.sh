#!/usr/bin/env bash
# Check that every MODULE_IMPORT_SYMBOL used in drivers has a corresponding
# EXPORT_SYMBOL in ksymtab.cpp. Intended to run at build time.
set -euo pipefail

KSYMTAB="src/kernel/src/ksymtab.cpp"
DRIVERS_DIR="src/drivers"

if [[ ! -f "$KSYMTAB" ]]; then
    echo "ERROR: $KSYMTAB not found" >&2
    exit 1
fi

# Extract exported symbols
exported=$(grep -oP 'EXPORT_SYMBOL\(\K[^)]+' "$KSYMTAB" | sort -u)

# Extract imported symbols from all driver modules
missing=0
for f in "$DRIVERS_DIR"/*/*.cpp; do
    imports=$(grep -oP 'MODULE_IMPORT_SYMBOL\(\K[^)]+' "$f" 2>/dev/null || true)
    for sym in $imports; do
        if ! echo "$exported" | grep -qx "$sym"; then
            echo "ERROR: $f imports '$sym' but no EXPORT_SYMBOL($sym) in $KSYMTAB" >&2
            missing=1
        fi
    done
done

if [[ $missing -eq 0 ]]; then
    echo "All module imports resolved."
else
    exit 1
fi
