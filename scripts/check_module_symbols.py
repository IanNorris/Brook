#!/usr/bin/env python3
"""Validate loadable kernel imports; called by check-module-symbols.sh."""
import os, re, subprocess, sys
from pathlib import Path

modules = []
if len(sys.argv) > 1:
    for arg in sys.argv[1:]:
        path = Path(arg)
        if path.is_dir():
            modules.extend(str(p) for p in sorted(path.rglob("*.mod")))
        elif path.is_file():
            modules.append(str(path))
else:
    modules = sorted(str(p) for p in Path("build").rglob("*.mod")
                     if "kernel/drivers/" in p.as_posix())
if not modules:
    print("check-module-symbols: no .mod files found (modules not built yet?) — skipping")
    sys.exit(0)

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
for mod in modules:
    if Path(mod).stat().st_size == 0:
        print(f"ERROR: {mod} is empty — relink the module", file=sys.stderr)
        errors += 1
        continue
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
      f"({len(modules)} modules, {len(exports)} kernel exports)")
