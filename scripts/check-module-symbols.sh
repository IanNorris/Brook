#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Build-time check: every undefined symbol referenced by a loadable kernel
# module (.mod) must be resolvable by the kernel's runtime module loader.
#
# WHY A BINARY CHECK (not a source check):
# The module loader (src/kernel/src/module.cpp:ResolveSymbol) walks each
# module's UNDEFINED ELF symbols and resolves them via KsymLookup() against
# the kernel's .ksymtab export table. If any UND symbol is missing from the
# export table the module fails to load at boot — e.g. BRO-154, where a driver
# called a kernel function that was never EXPORT_SYMBOL'd. That class of bug is
# invisible to a source-level scan of the MODULE_IMPORT_SYMBOL() markers
# (which are no-op documentation, see module_abi.h) because nothing forces the
# code's actual references to match the declared imports.
#
# This script instead inspects the compiled .mod binaries directly, so it
# catches every missing export regardless of whether the dependency was
# documented. It mirrors KsymLookup()'s resolution rules exactly, including the
# single-namespace C++ demangle fallback (ksymtab.cpp:TryDemangleBase).
#
# Usage:
#   check-module-symbols.sh [MODULE_DIR_OR_FILE ...]
# With no arguments it scans build/*/kernel/drivers/*.mod under the repo root.
# Exits non-zero (failing the build) if any module has an unresolvable symbol.
# ---------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$ROOT_DIR"

KSYMTAB="src/kernel/src/ksymtab.cpp"
if [[ ! -f "$KSYMTAB" ]]; then
    echo "ERROR: $KSYMTAB not found" >&2
    exit 1
fi

# Prefer llvm-nm, fall back to binutils nm.
NM_BIN="$(command -v llvm-nm || command -v nm || true)"
if [[ -z "$NM_BIN" ]]; then
    echo "ERROR: neither llvm-nm nor nm found on PATH" >&2
    exit 1
fi

# Use a named Python script: process substitution requires /dev/fd and large
# heredocs can use unlinked temporary files, both unsupported by Brook today.
KSYMTAB="$KSYMTAB" NM_BIN="$NM_BIN" exec python3 "$SCRIPT_DIR/check_module_symbols.py" "$@"
