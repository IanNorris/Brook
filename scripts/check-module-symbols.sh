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

# Collect module files from the arguments (files or directories). With no
# arguments, scan the conventional build output locations.
MODULES=()
if [[ $# -gt 0 ]]; then
    for arg in "$@"; do
        if [[ -d "$arg" ]]; then
            while IFS= read -r f; do MODULES+=("$f"); done \
                < <(find "$arg" -name '*.mod' | sort)
        elif [[ -f "$arg" ]]; then
            MODULES+=("$arg")
        fi
    done
else
    while IFS= read -r f; do MODULES+=("$f"); done \
        < <(find build -path '*/kernel/drivers/*.mod' 2>/dev/null | sort)
fi

if [[ ${#MODULES[@]} -eq 0 ]]; then
    echo "check-module-symbols: no .mod files found (modules not built yet?) — skipping"
    exit 0
fi

KSYMTAB="$KSYMTAB" NM_BIN="$NM_BIN" python3 - "${MODULES[@]}" <<'PYEOF'
import os, re, subprocess, sys

ksym = open(os.environ["KSYMTAB"]).read()
nm = os.environ["NM_BIN"]

# Build the set of names the kernel exports, matching what ends up in .ksymtab:
#   EXPORT_SYMBOL(x)              -> "x"
#   EXPORT_SYMBOL_NAMED(x, "n")   -> "n"
#   hand-rolled KernelSymbol{...} -> the first string-literal field (used for
#                                    volatile globals the macro can't express)
exports = set(re.findall(r'EXPORT_SYMBOL\(\s*([A-Za-z_]\w*)\s*\)', ksym))
exports |= {n for _, n in re.findall(
    r'EXPORT_SYMBOL_NAMED\(\s*([A-Za-z_]\w*)\s*,\s*"([^"]+)"', ksym)}
exports |= set(re.findall(r'KernelSymbol\b.*?=\s*\{\s*"([^"]+)"', ksym, re.DOTALL))

def demangle_base(s):
    # Faithful reimplementation of kernel TryDemangleBase (ksymtab.cpp):
    # parse _ZN <nsLen><ns> <fnLen><fn> ... and return the <fn> component.
    if not s.startswith("_ZN"):
        return None
    i, n = 3, 0
    while i < len(s) and s[i].isdigit():
        n = n * 10 + int(s[i]); i += 1
    if n == 0:
        return None
    i += n  # skip the namespace identifier
    f, j = 0, i
    while j < len(s) and s[j].isdigit():
        f = f * 10 + int(s[j]); j += 1
    if f == 0 or f > 127:
        return None
    return s[j:j + f]

def resolvable(sym):
    if sym in exports:
        return True
    base = demangle_base(sym)
    return bool(base and base in exports)

errors = 0
for mod in sys.argv[1:]:
    out = subprocess.run([nm, "-u", mod], capture_output=True, text=True)
    if out.returncode != 0:
        print(f"ERROR: {nm} failed on {mod}: {out.stderr.strip()}", file=sys.stderr)
        errors += 1
        continue
    for line in out.stdout.splitlines():
        parts = line.split()
        if not parts:
            continue
        sym = parts[-1]
        if not resolvable(sym):
            print(f"ERROR: {os.path.basename(mod)} references unresolved kernel "
                  f"symbol '{sym}' — add EXPORT_SYMBOL for it in ksymtab.cpp",
                  file=sys.stderr)
            errors += 1

if errors:
    print(f"check-module-symbols: {errors} unresolved symbol(s) — modules would "
          f"fail to load at boot", file=sys.stderr)
    sys.exit(1)
print(f"check-module-symbols: all module imports resolve "
      f"({len(exports)} kernel exports)")
PYEOF
