#!/usr/bin/env python3
"""Generate and validate a SteamRT provider cache from installed Nix exports.

Run in the guest. This does not modify the exports, library symlinks or /etc.
Publish the resulting cache as /etc/ld.so.cache separately during provisioning.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ldconfig', type=Path, required=True)
    parser.add_argument('--exports', type=Path, required=True)
    parser.add_argument('--include-directory', type=Path, action='append', default=[])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    tool = args.ldconfig.resolve(strict=True)
    directories = set()
    libraries = {}
    for export in sorted(args.exports.iterdir()):
        if '.so' not in export.name:
            continue
        target = export.resolve(strict=True)
        if not target.is_relative_to('/nix/store') or not target.is_file():
            raise RuntimeError(f'Export is not an installed Nix library: {export}')
        with target.open('rb') as stream:
            if stream.read(4) != b'\x7fELF':
                continue
        libraries[str(export)] = str(target)
        directories.add(target.parent)
    for directory in args.include_directory:
        directory = directory.resolve(strict=True)
        if not directory.is_relative_to('/nix/store') or not directory.is_dir():
            raise RuntimeError(f'Unapproved library directory: {directory}')
        directories.add(directory)
    if not directories:
        raise RuntimeError('No installed provider libraries')
    # An approved package can export another closure member through a symlink
    # (for example gcc's libgcc_s). Include its installed canonical directory.
    pending = list(directories)
    while pending:
        for entry in pending.pop().iterdir():
            if '.so' not in entry.name:
                continue
            target = entry.resolve(strict=True)
            if not target.is_file():
                continue
            if not target.is_relative_to('/nix/store'):
                raise RuntimeError(f'Provider library escapes Nix store: {entry}')
            with target.open('rb') as stream:
                is_elf = stream.read(4) == b'\x7fELF'
            if is_elf and target.parent not in directories:
                directories.add(target.parent)
                pending.append(target.parent)
    args.output.mkdir(parents=True, exist_ok=False)
    config = args.output / 'ld.so.conf'
    cache = args.output / 'ld.so.cache'
    config.write_text(''.join(str(p) + '\n' for p in sorted(directories)))
    command = [str(tool), '-X', '-i', '-C', str(cache), '-f', str(config)]
    with (args.output / 'stdout.log').open('wb') as stdout, (args.output / 'stderr.log').open('wb') as stderr:
        result = subprocess.run(command, stdout=stdout, stderr=stderr)
    if result.returncode:
        raise RuntimeError(f'ldconfig exited {result.returncode}; see {args.output}')
    listing = subprocess.run([str(tool), '-p', '-C', str(cache)], capture_output=True, check=True)
    (args.output / 'cache-list.txt').write_bytes(listing.stdout)
    targets = {}
    for line in listing.stdout.decode().splitlines():
        if ' => ' not in line:
            continue
        label, filename = line.strip().split(' => ', 1)
        path = Path(filename)
        target = path.resolve(strict=True)
        if not target.is_file() or not any(target.is_relative_to(directory) for directory in directories):
            raise RuntimeError(f'Cache target outside installed manifest: {path} -> {target}')
        targets[label] = str(target)
    if not targets:
        raise RuntimeError('Generated cache is empty')
    manifest = dict(command=command, tool=str(tool),
                    tool_sha256=hashlib.sha256(tool.read_bytes()).hexdigest(),
                    exports=libraries, directories=sorted(map(str, directories)),
                    targets=targets, cache=str(cache),
                    cache_sha256=hashlib.sha256(cache.read_bytes()).hexdigest())
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(dict(cache=str(cache), cache_sha256=manifest['cache_sha256'],
                          directories=len(directories), libraries=len(targets), validated=True)))


if __name__ == '__main__':
    main()
