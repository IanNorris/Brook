#!/usr/bin/env python3
"""Generate a sorted kernel symbol address table from an ELF binary.

Usage: gen_ksym_table.py <input_elf> <output_cpp>

Runs llvm-nm on the ELF to extract function symbols (T/t/W/w), optionally
demangles with llvm-cxxfilt, and emits a C++ source containing a sorted
array of {address, name} pairs for runtime address→symbol reverse lookup.

Two builds are needed for a complete table: the first build produces the ELF,
the POST_BUILD step generates the .cpp, and a second build links the real table.
"""

import subprocess
import sys
import shutil
import re
import os


def run(cmd, **kwargs):
    return subprocess.run(cmd, capture_output=True, text=True, **kwargs)


def parse_nm(elf_path):
    """Run llvm-nm and return [(addr, name)] for function symbols, sorted by addr."""
    nm = shutil.which("llvm-nm") or shutil.which("nm") or "llvm-nm"
    result = run([nm, "--defined-only", "-n", elf_path])
    if result.returncode != 0:
        print(f"llvm-nm failed: {result.stderr}", file=sys.stderr)
        sys.exit(1)

    func_types = set("TtWw")
    symbols = []
    for line in result.stdout.splitlines():
        parts = line.split(None, 2)
        if len(parts) != 3:
            continue
        addr_str, sym_type, name = parts
        if sym_type not in func_types:
            continue
        symbols.append((int(addr_str, 16), name))

    symbols.sort(key=lambda s: s[0])
    return symbols


def demangle(names):
    """Demangle a list of C++ names using llvm-cxxfilt if available."""
    cxxfilt = shutil.which("llvm-cxxfilt") or shutil.which("c++filt")
    if not cxxfilt:
        return names

    result = run([cxxfilt], input="\n".join(names))
    if result.returncode != 0:
        return names
    demangled = result.stdout.splitlines()
    if len(demangled) != len(names):
        return names
    return demangled


def clean_name(name):
    """Strip brook:: prefix and (anonymous namespace):: for brevity."""
    # Remove leading brook:: (possibly repeated after templates etc.)
    name = re.sub(r'\bbrook::', '', name)
    # Replace (anonymous namespace):: with nothing
    name = re.sub(r'\(anonymous namespace\)::', '', name)
    return name


def generate_cpp(symbols, output_path):
    """Write the C++ source file with the symbol table."""
    # Demangle all names in one batch
    mangled = [s[1] for s in symbols]
    names = demangle(mangled)
    names = [clean_name(n) for n in names]

    with open(output_path, "w") as f:
        f.write("// Auto-generated kernel symbol table — do not edit\n")
        f.write('#include "ksym_addrs.h"\n\n')
        f.write("namespace brook {\n")
        f.write("extern const KsymAddrEntry g_ksymAddrTable[] = {\n")
        for (addr, _mangled), name in zip(symbols, names):
            escaped = name.replace("\\", "\\\\").replace('"', '\\"')
            f.write(f'    {{ 0x{addr:016x}ULL, "{escaped}" }},\n')
        if not symbols:
            f.write('    { 0, "(none)" },\n')
        f.write("};\n")
        if symbols:
            f.write("extern const uint32_t g_ksymAddrCount = "
                    "sizeof(g_ksymAddrTable)/sizeof(g_ksymAddrTable[0]);\n")
        else:
            f.write("extern const uint32_t g_ksymAddrCount = 0;\n")
        f.write("} // namespace brook\n")

    print(f"gen_ksym_table: {len(symbols)} function symbols written to {output_path}")


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input_elf> <output_cpp>", file=sys.stderr)
        sys.exit(1)

    elf_path = sys.argv[1]
    output_path = sys.argv[2]

    if not os.path.isfile(elf_path):
        print(f"ELF not found: {elf_path}", file=sys.stderr)
        sys.exit(1)

    symbols = parse_nm(elf_path)
    generate_cpp(symbols, output_path)


if __name__ == "__main__":
    main()
