#!/usr/bin/env python3
"""Generate a compact package index for Brook's Nix package manager.

Two-phase approach:
  1. nix-instantiate to batch-eval store paths for all top-level nixpkgs attrs
  2. nix-env -qaP --json --meta for descriptions and metadata

Output: TSV file with name, version, store_name, description and an optional
fifth main_program column (meta.mainProgram) per line. Only derivations whose
`.system` is the target system (x86_64-linux) are offered: Brook loads only
x86-64 ELF images, so e.g. zsnes (i686-linux) is recorded in
<output>.excluded.tsv with a reason instead of being silently dropped. Unfree
packages (e.g. snes9x) are recorded there too: the pin is evaluated with
allowUnfree = false and Hydra does not build them, so the binary cache has
nothing to install.

Usage:
    python3 gen-nix-index.py [--output packages.idx]
    python3 gen-nix-index.py --help
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import hashlib
from pathlib import Path

NIX_STORE_PATHS_EXPR = r'''
  let
    pkgs = import @PACKAGE_SET@;
    names = builtins.attrNames pkgs;
    getInfo = name:
      let
        raw = builtins.tryEval pkgs.${name};
        hasDrv = raw.success
          && builtins.tryEval (raw.value ? outPath)
             == { success = true; value = true; };
        path = if hasDrv
          then builtins.tryEval (builtins.toString raw.value.outPath)
          else { success = false; value = ""; };
        system = builtins.tryEval (if raw.value ? system then builtins.toString raw.value.system else "");
        # Unfree packages throw on outPath (allowUnfree = false) and Hydra
        # does not build them, so the binary cache cannot serve them either.
        unfree = raw.success && (builtins.tryEval (raw.value.meta.unfree or false)) == { success = true; value = true; };
      in if path.success then {
        inherit name;
        value = { path = path.value; system = if system.success then system.value else ""; };
      } else if hasDrv && unfree then {
        inherit name;
        value = { path = ""; system = if system.success then system.value else ""; unfree = true; };
      } else null;
    results = builtins.filter (x: x != null) (map getInfo names);
  in builtins.listToAttrs results
'''


def eval_store_paths(package_set: Path) -> dict[str, dict]:
    """Evaluate all top-level nixpkgs store paths. Returns attr -> {path, system}."""
    print("Phase 1: Evaluating store paths (may take 1-2 min)...", file=sys.stderr)
    result = subprocess.run(
        ["nix-instantiate", "--eval", "--strict", "--json", "-E",
         NIX_STORE_PATHS_EXPR.replace('@PACKAGE_SET@', str(package_set))],
        capture_output=True, text=True, timeout=600
    )
    if result.returncode != 0:
        print(f"nix-instantiate failed: {result.stderr[:500]}", file=sys.stderr)
        sys.exit(1)
    return json.loads(result.stdout)


def query_metadata(package_set: Path) -> dict:
    """Query nixpkgs for metadata (descriptions). Returns attr -> info dict."""
    print("Phase 2: Querying package metadata...", file=sys.stderr)
    result = subprocess.run(
        ["nix-env", "-f", str(package_set), "-qaP", "--json", "--meta"],
        capture_output=True, text=True, timeout=600
    )
    if result.returncode != 0:
        print(f"nix-env failed: {result.stderr[:500]}", file=sys.stderr)
        sys.exit(1)
    return json.loads(result.stdout)


def store_name_from_path(store_path: str) -> str:
    """Extract hash-name from /nix/store/hash-name."""
    parts = store_path.rstrip("/").split("/")
    return parts[3] if len(parts) >= 4 else ""


def main():
    parser = argparse.ArgumentParser(description="Generate Brook Nix package index")
    parser.add_argument("--output", "-o", default="packages.idx",
                        help="Output index file (default: packages.idx)")
    parser.add_argument('--nixpkgs', type=Path,
                        default=Path(__file__).resolve().parents[2]/'nix/nixpkgs.nix',
                        help='Pinned nixpkgs import function; defaults to Brook\'s pin')
    parser.add_argument('--system', default='x86_64-linux')
    args = parser.parse_args()

    pin = args.nixpkgs.resolve(strict=True)
    # Both phases import exactly the same explicit configuration. Never join
    # paths from a host channel with metadata from a different package set.
    quoted_pin = json.dumps(str(pin)).replace('${', '\\${')
    quoted_system = json.dumps(args.system).replace('${', '\\${')
    expression = f'import (builtins.toPath {quoted_pin}) {{ system = {quoted_system}; config.allowUnfree = false; }}\n'
    with tempfile.TemporaryDirectory(prefix='brook-nix-index-') as scratch:
        package_set = Path(scratch)/'packages.nix'
        package_set.write_text(expression)
        store_paths = eval_store_paths(package_set)
        print(f"  Got {len(store_paths)} store paths", file=sys.stderr)
        metadata = query_metadata(package_set)
    print(f"  Got {len(metadata)} metadata entries", file=sys.stderr)

    entries, excluded = build_entries(store_paths, metadata, args.system)
    write_index(Path(args.output), entries, excluded, pin, args.system)


def clean_field(value) -> str:
    return " ".join(str(value or "").split())


def build_entries(store_paths: dict, metadata: dict, system: str):
    """Join phase 1 and 2. Returns (entries, excluded) row lists."""
    entries = []
    excluded = []
    for attr, info in store_paths.items():
        if isinstance(info, str):
            info = {'path': info, 'system': ''}
        store_path = info.get('path', '')
        store_name = store_name_from_path(store_path)
        meta_info = metadata.get(attr, {})
        if info.get('unfree'):
            excluded.append((meta_info.get('pname', attr), meta_info.get('version', ''), attr,
                             'unfree license; not built by Hydra, so the binary cache cannot serve it'))
            continue
        if not store_name:
            continue

        pname = meta_info.get("pname", attr)
        version = meta_info.get("version", "")
        description = meta_info.get("meta", {}).get("description", "")
        if description:
            description = " ".join(description.split())

        # If no metadata entry, try to extract version from store name
        if not version and "-" in store_name:
            # hash-name-version -> try to get version
            name_ver = store_name.split("-", 1)[1] if "-" in store_name else ""
            # Version is typically the last dash-separated part that starts with a digit
            parts = name_ver.rsplit("-", 1)
            if len(parts) == 2 and parts[1] and parts[1][0].isdigit():
                version = parts[1]
                if not pname or pname == attr:
                    pname = parts[0]

        main_program = clean_field(meta_info.get("meta", {}).get("mainProgram", ""))
        if "/" in main_program or main_program.startswith("."):
            main_program = ""
        drv_system = info.get('system') or meta_info.get('system', '')
        if drv_system != system:
            reason = f"unsupported system {drv_system or 'unknown'}; Brook runs {system} binaries only"
            excluded.append((pname, version, attr, reason))
            continue
        entries.append((pname, version, store_name, description, main_program))

    # Sort by lowercase package name
    entries.sort(key=lambda e: e[0].lower())

    # Deduplicate: keep first entry per (pname, version)
    seen = set()
    unique = []
    for entry in entries:
        key = (entry[0], entry[1])
        if key not in seen:
            seen.add(key)
            unique.append(entry)
    entries = unique
    # A package name offered for this system is not also "excluded".
    offered = {e[0] for e in entries}
    excluded = sorted({e for e in excluded if e[0] not in offered}, key=lambda e: (e[0].lower(), e[2]))
    return entries, excluded


def write_index(output: Path, entries, excluded, pin: Path, system: str):
    # Write TSV: name\tversion\tstore_name\tdescription[\tmain_program]
    with open(output, "w") as f:
        for name, version, store_name, desc, main_program in entries:
            row = f"{name}\t{version}\t{store_name}\t{desc}"
            if main_program:
                row += f"\t{main_program}"
            f.write(row + "\n")
    excluded_path = Path(str(output) + '.excluded.tsv')
    with open(excluded_path, "w") as f:
        f.write("# name\tversion\tattribute\treason (not offered by the Gallery)\n")
        for row in excluded:
            f.write("\t".join(clean_field(x) for x in row) + "\n")

    size_mb = os.path.getsize(output) / (1024 * 1024)
    print(f"Wrote {len(entries)} packages to {output} ({size_mb:.1f} MB); "
          f"excluded {len(excluded)} (other systems or unfree)", file=sys.stderr)
    try: pin_label = str(pin.relative_to(Path(__file__).resolve().parents[2]))
    except ValueError: pin_label = str(pin)
    Path(str(output)+'.json').write_text(json.dumps({
        'format': 2, 'columns': ['name', 'version', 'store_name', 'description', 'main_program?'],
        'nixpkgs_import': pin_label,
        'nixpkgs_import_sha256': hashlib.sha256(pin.read_bytes()).hexdigest(),
        'system': system, 'allow_unfree': False, 'entries': len(entries),
        'excluded': len(excluded),
        'excluded_other_system': sum(1 for e in excluded if e[3].startswith('unsupported system')),
        'excluded_unfree': sum(1 for e in excluded if e[3].startswith('unfree')),
        'index_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
        'binary_cache_availability_checked': False,
    }, indent=2)+'\n')


if __name__ == "__main__":
    main()
