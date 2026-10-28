#!/usr/bin/env python3
"""Verify and ZIP a clean portable candidate, excluding assembly/test state.

Nix files commonly have 1970 timestamps. ZIP requires 1980 or later; clamp
timestamps while preserving executable modes. QEMU disk clusters are already
compressed, so store that file directly rather than compressing it twice.
"""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--with-tools', action='store_true', help='Include optional MCP, installer and development/review files')
    args = parser.parse_args()
    bundle = args.bundle.resolve()
    manifest = json.loads((bundle / 'manifest.json').read_text())
    required = ['brook.qcow2', 'OVMF_CODE.fd', 'OVMF_VARS.fd', 'launch.py',
                'run-linux.sh', 'run-macos.command', 'run-windows.cmd', 'README.txt',
                'BROOK-LICENSE.txt']
    if args.with_tools:
        required += ['mcp.py', 'software.json', 'manifest.json']
    for name, expected in manifest['files'].items():
        if Path(name).name != name or digest(bundle / name) != expected:
            raise ValueError('Candidate integrity mismatch: ' + name)
    paths = [bundle / name for name in required]
    for name in ['ACCEPTANCE.txt', 'SOFTWARE-AND-TESTS.md', 'RELEASE-NOTES.txt', 'BROOK-CHANGES.patch']:
        if args.with_tools and (bundle / name).exists():
            paths.append(bundle / name)
    for name in (['installers', 'linux-development-bridge'] if args.with_tools else []):
        for path in sorted((bundle / name).rglob('*')):
            if '__pycache__' not in path.parts and path.is_file() and path.suffix in ('.py', '.sh', '.json', '.nix', '.md', '.txt'):
                if path.name == 'session.json' or path.is_symlink():
                    raise ValueError('Unexpected session or symlink in source payload: ' + str(path))
                paths.append(path)
    with zipfile.ZipFile(args.output, 'x', allowZip64=True, strict_timestamps=False) as archive:
        for path in paths:
            if path.is_symlink():
                raise ValueError('Bundle payload must contain regular files')
            archive.write(path, Path('Brook') / path.relative_to(bundle),
                          compress_type=zipfile.ZIP_STORED if path.suffix == '.qcow2' else zipfile.ZIP_DEFLATED)
    with zipfile.ZipFile(args.output) as archive:
        if archive.testzip() is not None:
            raise ValueError('ZIP integrity check failed')
    print('Verified archive:', args.output, args.output.stat().st_size, 'bytes')


if __name__ == '__main__':
    main()
