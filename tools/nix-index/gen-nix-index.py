#!/usr/bin/env python3
"""Generate a compact package index for Brook's Nix package manager.

Two-phase approach:
  1. nix-instantiate to batch-eval store paths for all top-level nixpkgs attrs
  2. nix-env -qaP --json --meta for descriptions and metadata

Output: TSV file with name, version, store_name, description per line.

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
      in if path.success then { inherit name; value = path.value; } else null;
    results = builtins.filter (x: x != null) (map getInfo names);
  in builtins.listToAttrs results
'''


def eval_store_paths(package_set: Path) -> dict[str, str]:
    """Evaluate all top-level nixpkgs store paths. Returns attr -> store_path."""
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

    # Build index by joining store paths with metadata
    entries = []
    for attr, store_path in store_paths.items():
        store_name = store_name_from_path(store_path)
        if not store_name:
            continue

        meta_info = metadata.get(attr, {})
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

        entries.append((pname, version, store_name, description))

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

    # Write TSV: name\tversion\tstore_name\tdescription
    with open(args.output, "w") as f:
        for name, version, store_name, desc in entries:
            f.write(f"{name}\t{version}\t{store_name}\t{desc}\n")

    size_mb = os.path.getsize(args.output) / (1024 * 1024)
    print(f"Wrote {len(entries)} packages to {args.output} ({size_mb:.1f} MB)",
          file=sys.stderr)
    try: pin_label = str(pin.relative_to(Path(__file__).resolve().parents[2]))
    except ValueError: pin_label = str(pin)
    Path(str(args.output)+'.json').write_text(json.dumps({
        'format': 1, 'nixpkgs_import': pin_label,
        'nixpkgs_import_sha256': hashlib.sha256(pin.read_bytes()).hexdigest(),
        'system': args.system, 'allow_unfree': False, 'entries': len(entries),
        'index_sha256': hashlib.sha256(Path(args.output).read_bytes()).hexdigest(),
        'binary_cache_availability_checked': False,
    }, indent=2)+'\n')


if __name__ == "__main__":
    main()
