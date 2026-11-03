#!/usr/bin/env python3
"""Download Valve's 64-bit Steam client into a new private Brook home.

The official manifest supplies the package sizes and SHA-256 hashes. This
stages a client; it does not sign in, download games, or launch Steam.
"""
import argparse
import ctypes
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import struct
import subprocess
import tempfile
import zipfile

BASE = 'https://client-update.steamstatic.com/'
MANIFEST = BASE + 'steam_client_steamrt64'
FILENAME = re.compile(r'^[A-Za-z0-9._-]+$')
HEX = re.compile(r'^[0-9a-f]{64}$')
MAX_EXPANDED = 16 * 1024**3
MAX_MEMBERS = 250000


def parse_manifest(text):
    packages = []
    for name, body in re.findall(r'"([^"\n]+)"\s*\{([^{}]+)\}', text):
        fields = dict(re.findall(r'"([^"\n]+)"\s+"([^"\n]+)"', body))
        if 'file' not in fields or name == 'runtime_scout_steamrt64':
            continue
        file, size, digest = fields['file'], fields.get('size', ''), fields.get('sha2', '')
        if not FILENAME.fullmatch(file) or not size.isdigit() or not HEX.fullmatch(digest):
            raise ValueError('invalid Steam package metadata')
        size = int(size)
        if not 0 < size <= 512 * 1024 * 1024:
            raise ValueError('unexpected Steam package size')
        packages.append((name, file, size, digest))
    if not packages or not any(name == 'steam_steamrt64' for name, *_ in packages):
        raise ValueError('manifest does not describe the expected 64-bit client')
    if len(packages) > 128 or len({p[1] for p in packages}) != len(packages):
        raise ValueError('unexpected duplicate or excessive Steam packages')
    return packages


def confined(root, path):
    resolved = path.resolve(strict=False)
    if not resolved.is_relative_to(root):
        raise ValueError('archive path escapes the client directory')
    return path


def extract(archive, root, expanded=0, members=0):
    root = root.resolve(strict=True)
    with zipfile.ZipFile(archive) as z:
        entries = z.infolist()
        members += len(entries)
        expanded += sum(item.file_size for item in entries)
        if members > MAX_MEMBERS or expanded > MAX_EXPANDED:
            raise ValueError('Steam packages exceed extraction limits')
        space = os.statvfs(root)
        if sum(item.file_size for item in entries) + 1024**3 > space.f_bavail * space.f_frsize:
            raise ValueError('insufficient space for Steam package extraction')
        for item in entries:
            name = item.filename.replace('\\', '/')
            parts = PurePosixPath(name).parts
            if not parts:
                continue
            if name.startswith('/') or any(x in ('.', '..') for x in parts):
                raise ValueError('invalid archive path')
            if parts[0] in ('ubuntu12_32', 'linux32', 'steamrt32'):
                continue
            path = confined(root, root.joinpath(*parts))
            path.parent.mkdir(parents=True, exist_ok=True)
            mode = item.external_attr >> 16
            if item.is_dir():
                path.mkdir(exist_ok=True)
            elif stat.S_ISLNK(mode):
                target = z.read(item).decode('utf-8')
                if os.path.isabs(target):
                    raise ValueError('absolute archive symlink')
                confined(root, path.parent / target)
                if path.is_symlink() or path.exists():
                    path.unlink()
                path.symlink_to(target)
            else:
                if path.is_symlink():
                    path.unlink()
                with z.open(item) as src, path.open('wb') as dst:
                    shutil.copyfileobj(src, dst, 1024 * 1024)
                with path.open('rb') as f:
                    head = f.read(4)
                path.chmod(0o755 if head == b'\x7fELF' or head.startswith(b'#!') else 0o644)
    return expanded, members


def has_interpreter(path):
    with path.open('rb') as f:
        head = f.read(64)
        if len(head) != 64 or head[:6] != b'\x7fELF\x02\x01':
            return False
        phoff = struct.unpack_from('<Q', head, 32)[0]
        entsize, count = struct.unpack_from('<HH', head, 54)
        if entsize != 56 or count > 1024:
            raise ValueError('unsupported ELF program headers')
        f.seek(phoff)
        for _ in range(count):
            ph = f.read(entsize)
            if len(ph) != entsize:
                raise ValueError('truncated ELF program header')
            if struct.unpack_from('<I', ph)[0] == 3:
                return True
    return False


def download(curl, url, path, maximum):
    subprocess.run([curl, '--fail', '--location', '--silent', '--show-error',
                    '--connect-timeout', '30', '--max-time', '1800',
                    '--retry', '3', '--cacert', '/nix/etc/ssl/certs/ca-bundle.crt',
                    '--max-filesize', str(maximum),
                    '--output', str(path), url], check=True)


def publish(stage, home):
    libc = ctypes.CDLL(None, use_errno=True)
    rename = libc.renameat2
    rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    rename.restype = ctypes.c_int
    if rename(-100, os.fsencode(stage), -100, os.fsencode(home), 1):
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(home))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--home', type=Path, default=Path('/data/home/steam-20261022'))
    p.add_argument('--curl', default='/nix/bin/curl')
    p.add_argument('--patchelf', default='/nix/profile/bin/patchelf')
    p.add_argument('--loader', required=True, help='Installed 64-bit Nix dynamic loader')
    args = p.parse_args()
    if os.geteuid() == 0:
        raise SystemExit('Run the Steam bootstrap as the desktop user, not root')
    home = args.home.absolute()
    if home.exists() or home.is_symlink():
        raise SystemExit('Steam home already exists; leave it intact')
    if not Path(args.loader).is_file() or not os.access(args.patchelf, os.X_OK):
        raise SystemExit('Install patchelf and provide an existing dynamic loader first')
    home.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.brook-steam-bootstrap-', dir=home.parent))
    print('Private staging directory:', stage, flush=True)
    client = stage / 'client'; client.mkdir()
    manifest = stage / 'manifest'; download(args.curl, MANIFEST, manifest, 1024 * 1024)
    if manifest.stat().st_size > 1024 * 1024:
        raise ValueError('Steam manifest too large')
    packages = parse_manifest(manifest.read_text())
    expanded = members = 0
    for name, file, size, digest in packages:
        print('Downloading', name, flush=True)
        package = stage / 'package.zip'; download(args.curl, BASE + file, package, size)
        with package.open('rb') as f:
            actual = hashlib.file_digest(f, 'sha256').hexdigest()
        if package.stat().st_size != size or actual != digest:
            raise ValueError('Steam package size or hash mismatch: ' + name)
        expanded, members = extract(package, client, expanded, members); package.unlink()
    patched = 0
    for path in client.rglob('*'):
        if path.is_symlink() or not path.is_file():
            continue
        if has_interpreter(path):
            subprocess.run([args.patchelf, '--set-interpreter', args.loader, str(path)], check=True)
            patched += 1
        else:
            with path.open('rb') as f:
                first = f.readline(256)
            if first.rstrip(b'\r\n') in (b'#!/bin/bash', b'#!/usr/bin/env bash'):
                data = path.read_bytes()
                path.write_bytes(b'#!/nix/profile/bin/bash\n' + data[len(first):])
    if not (client / 'steamrt64/steam').is_file():
        raise ValueError('Steam client executable is absent')
    hostlibs = client / 'hostlibs'; hostlibs.mkdir(exist_ok=True)
    exports = Path('/nix/profile/lib')
    if exports.is_dir():
        for path in exports.iterdir():
            if '.so' in path.name and (path.exists() or path.is_symlink()):
                target = hostlibs / path.name
                if not target.exists() and not target.is_symlink():
                    target.symlink_to(path)
    publish(stage, home)
    print('STEAM_BOOTSTRAP_READY', home, 'packages', len(packages), 'patched', patched, flush=True)


if __name__ == '__main__':
    main()
