#!/usr/bin/env python3
"""Read the built Brook kernel's app-facing capabilities."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


SCHEMA = "brook-kernel-capabilities/1"
SECTION_NAME = b".brook_capabilities\0"
ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
SECTION_HEADER = struct.Struct("<IIQQQQIIQQ")
MAX_DECLARATION_BYTES = 4096


def _declaration(elf):
    if len(elf) < ELF_HEADER.size:
        raise ValueError("truncated kernel ELF")
    ident, kind, machine, _, _, _, sections, _, _, _, _, size, count, names = ELF_HEADER.unpack_from(elf)
    if ident[:7] != b"\x7fELF\x02\x01\x01" or kind != 2 or machine != 62:
        raise ValueError("expected a little-endian x86-64 kernel executable")
    if (size != SECTION_HEADER.size or not 0 < names < count
            or sections < ELF_HEADER.size or sections + size * count > len(elf)):
        raise ValueError("invalid kernel ELF section table")

    def read_section(index):
        return SECTION_HEADER.unpack_from(elf, sections + index * size)

    _, names_type, _, _, names_offset, names_size, *_ = read_section(names)
    if names_type != 3 or names_offset + names_size > len(elf):
        raise ValueError("invalid kernel ELF section names")

    declaration = None
    names_end = names_offset + names_size
    for i in range(1, count):
        name, kind, flags, _, offset, length, *_ = read_section(i)
        name_offset = names_offset + name
        if not elf.startswith(SECTION_NAME, name_offset, names_end):
            continue
        if (declaration is not None or kind != 1 or flags != 0
                or not 1 < length <= MAX_DECLARATION_BYTES or offset + length > len(elf)):
            raise ValueError("invalid kernel capabilities section")
        declaration = elf[offset:offset + length]

    if declaration is None:
        raise ValueError("kernel does not declare app capabilities")
    if not declaration.endswith(b"\0"):
        raise ValueError("unterminated kernel capabilities declaration")
    return declaration[:-1]


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate kernel capability field: {key}")
        result[key] = value
    return result


def _validate_versions(versions):
    if not isinstance(versions, dict):
        raise ValueError("kernel capabilities must map identifiers to versions")
    for name, version in versions.items():
        if (not isinstance(name, str) or not re.fullmatch(r"[a-z][a-z0-9-]*", name)
                or type(version) is not int or version < 1):
            raise ValueError(f"invalid kernel capability: {name!r} = {version!r}")


def read_kernel_capabilities(kernel):
    """Extract declarations and hash the same bytes of the built kernel."""
    elf = Path(kernel).read_bytes()
    capabilities = json.loads(_declaration(elf), object_pairs_hook=_unique_object)
    _validate_versions(capabilities)
    return {
        "schema": SCHEMA,
        "kernel_sha256": hashlib.sha256(elf).hexdigest(),
        "capabilities": capabilities,
    }


def verify_kernel_capabilities(kernel, manifest):
    """Return the built kernel's manifest after validating its sidecar."""
    expected = read_kernel_capabilities(kernel)
    published = json.loads(Path(manifest).read_bytes(), object_pairs_hook=_unique_object)
    if not isinstance(published, dict) or "capabilities" not in published:
        raise ValueError("invalid kernel capability manifest")
    _validate_versions(published["capabilities"])
    if published != expected:
        raise ValueError("kernel capability manifest does not match the built kernel")
    return expected


def require_kernel_capabilities(manifest, requirements):
    """Require the exact ABI version for each requested app contract."""
    _validate_versions(requirements)
    available = manifest["capabilities"]
    for name, required in sorted(requirements.items()):
        actual = available.get(name)
        if actual != required:
            detail = "missing" if actual is None else f"available version {actual}"
            raise ValueError(f"app requires kernel capability {name} version {required} ({detail})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        content = json.dumps(read_kernel_capabilities(args.kernel), indent=2, sort_keys=True) + "\n"
        if args.output:
            args.output.write_text(content)
        else:
            print(content, end="")
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
