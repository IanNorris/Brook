#!/usr/bin/env python3
"""Build a guest package and export its closure to an ignored local Nix cache."""
import argparse
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parent.parent

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--expression', type=Path, default=ROOT/'tools/vlc-brook-pkg')
    parser.add_argument('--output', type=Path, default=ROOT/'build/package-cache')
    parser.add_argument('--name', default='vlc-brook-native')
    parser.add_argument('--version', default='3.0.23-2')
    args = parser.parse_args()
    for value in (args.name, args.version):
        if not re.fullmatch(r'[a-zA-Z0-9_.+-]+', value):
            parser.error('name and version must be single index fields')
    destination = args.output.resolve()
    # Package caches are build artifacts, including their potentially huge NARs.
    if not destination.is_relative_to(ROOT/'build'):
        parser.error('--output must be beneath the repository build directory')
    result = subprocess.run(['nix-build', str(args.expression.resolve()), '--no-out-link'],
                            check=True, text=True, stdout=subprocess.PIPE)
    store_path = result.stdout.strip()
    if '\n' in store_path or not store_path.startswith('/nix/store/'):
        raise RuntimeError('expression must produce exactly one store output')
    subprocess.run(['nix', '--extra-experimental-features', 'nix-command', 'copy',
                    '--to', destination.as_uri()+'?compression=zstd', store_path], check=True)
    index = f'{args.name}\t{args.version}\t{Path(store_path).name}\tBrook compatibility package\n'
    (destination/(args.name+'.idx')).write_text(index)
    (destination/(args.name+'.json')).write_text(json.dumps({
        'name': args.name, 'version': args.version, 'store_path': store_path,
        'expression': str(args.expression.resolve())}, indent=2)+'\n')
    print('Cache:', destination)
    print('Guest index:', args.name+'.idx')
    print('Store path:', store_path)

if __name__ == '__main__':
    main()
