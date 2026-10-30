#!/usr/bin/env python3
"""Build a bounded writable /nix ext2 image; no mounts or physical devices.

The profile/closure comes from the pinned Nix expression. Profile directories
are materialized so installing packages never writes through a buildEnv
directory symlink into the store. Package data remains under /nix/store.
Only new files under a new output directory are created. Interrupted output
is retained for inspection, never reused or automatically retried.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import uuid

from ext2_image_export import Source

ROOT = Path(__file__).resolve().parents[1]
FAT_FILE_MAX = 0xfffffe00


def run(*argv, **kwargs):
    return subprocess.run(list(map(str, argv)), check=True, cwd=ROOT, **kwargs)


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def profile_tree(source, target, closure, ancestors=()):
    """Real directories, absolute store-file links; reject escapes and cycles."""
    resolved = source.resolve(strict=True)
    if resolved in ancestors or len(ancestors) >= 128:
        raise ValueError('Profile directory cycle/depth: '+str(source))
    if resolved.parts[:3] != ('/', 'nix', 'store') or resolved.parts[3] not in closure:
        raise ValueError('Profile reference outside closure: '+str(source))
    if resolved.is_dir():
        target.mkdir(mode=0o755)
        for child in sorted(source.iterdir()):
            profile_tree(child, target/child.name, closure, ancestors+(resolved,))
    elif resolved.is_file():
        target.symlink_to(resolved)
    else:
        raise ValueError('Unsupported profile entry: '+str(source))


def mutable(path):
    return path == 'store' or any(path == root or path.startswith(root+'/')
        for root in ('profile', 'var', 'home/brook', 'share/applications'))


def normalize_owners(image, output):
    # Inode numbers avoid sending arbitrary package filenames through debugfs's
    # command parser. Close the read-only library view before opening a writer.
    source = Source(image)
    try:
        report = source.inventory('/')
    finally:
        source.close()
    owners = {}
    for name, meta in report['nodes'].items():
        owner = 1000 if mutable(name) else 0
        previous = owners.setdefault(meta['ino'], owner)
        if previous != owner:
            raise ValueError('Hardlink crosses mutable/store ownership boundary')
    commands = output/'owners.debugfs'
    with commands.open('x') as stream:
        for ino, owner in sorted(owners.items()):
            stream.write(f'set_inode_field <{ino}> uid {owner}\n')
            stream.write(f'set_inode_field <{ino}> gid {owner}\n')
    with (output/'owners.log').open('x') as log:
        run('debugfs', '-w', '-f', commands, image, stdout=log, stderr=subprocess.STDOUT)
    # debugfs may report command failures without a failing exit status.
    source = Source(image)
    try:
        checked = source.inventory('/')
        for name, meta in checked['nodes'].items():
            owner = 1000 if mutable(name) else 0
            if meta['uid'] != owner or meta['gid'] != owner:
                raise ValueError('Image ownership normalization failed: '+name)
    finally:
        source.close()
    commands.unlink()
    return checked


def remove_seed(seed):
    # This exclusively-owned build scratch is the only tree removed. Never
    # follow package symlinks into the host's Nix store during chmod/cleanup.
    for directory, _, _ in os.walk(seed, followlinks=False):
        os.chmod(directory, 0o700)
    shutil.rmtree(seed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--expression', type=Path, default=ROOT/'nix/hardware-network-packages.nix')
    parser.add_argument('--index', type=Path, default=ROOT/'tools/nix-index/packages.idx')
    parser.add_argument('--size-mib', type=int, default=3840)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    profile = args.profile.resolve(strict=True)
    expression = args.expression.resolve(strict=True)
    index = args.index.resolve(strict=True)
    size = args.size_mib*1024*1024
    if profile.parent != Path('/nix/store') or not profile.is_dir():
        parser.error('Profile must resolve to a built Nix store directory')
    if not 512*1024*1024 <= size <= FAT_FILE_MAX:
        parser.error('Image must be 512 MiB..FAT32 file limit')
    compiler = os.environ.get('MUSL_CC')
    if not compiler:
        parser.error('Run inside nix-shell with MUSL_CC')
    output = args.output.absolute()
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    closure = subprocess.check_output(['nix-store', '-qR', str(profile)], text=True).splitlines()
    if not closure or any(Path(p).parent != Path('/nix/store') for p in closure):
        raise ValueError('Invalid Nix closure')
    nix = ('let e = import (builtins.toPath '+json.dumps(str(expression))+') {}; in '
           '{ profile = toString e.profile; packages = map (p: { '
           'name = p.pname or p.name; version = p.version or ""; '
           'store = builtins.baseNameOf (toString p); }) e.packages; }')
    metadata = json.loads(subprocess.check_output(
        ['nix-instantiate', '--eval', '--strict', '--json', '-E', nix], text=True))
    if metadata['profile'] != str(profile):
        raise ValueError('Expression and profile do not match')
    (output/'closure.txt').write_text('\n'.join(sorted(closure))+'\n')
    seed = output/'seed';(seed/'store').mkdir(parents=True)
    for item in closure:
        run('cp', '-a', '--reflink=auto', '--', item, seed/'store'/Path(item).name)
    profile_tree(profile, seed/'profile', {Path(p).name for p in closure})
    for directory in ('bin', 'index', 'etc/ssl/certs', 'var/cache/narinfo',
                      'home/brook/.config/brook', 'home/brook/.cache',
                      'home/brook/.local/share', 'share/applications'):
        (seed/directory).mkdir(parents=True, exist_ok=True)
    tools = {}
    for name in ('nix-install', 'nix-fetch', 'nix-search', 'nar-unpack'):
        source = ROOT/'tools'/name/(name+'.c')
        run(compiler, '-static', '-O2', '-Wall', '-Wextra', source, '-o', seed/'bin'/name)
        tools[name] = {'source_sha256': digest(source), 'binary_sha256': digest(seed/'bin'/name)}
    (seed/'bin/nix').symlink_to('nix-install')
    for name in ('curl', 'xz', 'zstd'):
        if not (profile/'bin'/name).is_file():
            raise ValueError('Required runtime helper missing: '+name)
        (seed/'bin'/name).symlink_to('../profile/bin/'+name)
    ca = profile/'etc/ssl/certs/ca-bundle.crt'
    if not ca.is_file():
        raise ValueError('Profile CA bundle missing')
    (seed/'etc/ssl/certs/ca-bundle.crt').symlink_to('/nix/profile/etc/ssl/certs/ca-bundle.crt')
    shutil.copyfile(index, seed/'index/packages.idx')
    rows = []
    for p in metadata['packages']:
        fields = [p['name'], p['version'], p['store']]
        if any('\n' in f or '\t' in f for f in fields):
            raise ValueError('Invalid package manifest field')
        rows.append('\t'.join(fields))
    (seed/'profile/manifest.tsv').write_text('\n'.join(sorted(set(rows)))+'\n')
    # collect_desktop_entries resolves guest /nix paths against the seed and
    # uses only the profile exports, avoiding uninstalled closure-only entries.
    run('python3', ROOT/'scripts/collect_desktop_entries.py', seed/'store', seed/'share/applications')
    manifest = seed/'share/applications/applications.idx'
    records = []
    for line in manifest.read_text().splitlines():
        fields = line.split('\t')
        for name in ('mousepad', 'brook-files', 'brook-settings', 'wayland-calc'):
            if fields[1].split(' ', 1)[0] in (name, '/nix/profile/bin/'+name):
                fields[1] = '/nix/profile/bin/hardware-'+name
        records.append('\t'.join(fields))
    manifest.write_text('\n'.join(records)+'\n')
    (seed/'BROOK.MNT').write_text('/nix\n')
    identity = {'profile': str(profile), 'closure': sorted(closure),
                'index_sha256': digest(index), 'tools': tools,
                'builder_sha256': digest(Path(__file__))}
    identifier = str(uuid.uuid5(uuid.NAMESPACE_URL, json.dumps(identity, sort_keys=True)))
    (seed/'.brook-store.json').write_text(json.dumps({'format': 1, 'uuid': identifier, **identity}, indent=2)+'\n')
    image = output/'STORE.IMG'
    with image.open('xb') as stream:
        stream.truncate(size)
    run('mke2fs', '-q', '-t', 'ext2', '-b', '4096', '-I', '128', '-m', '0',
        '-N', '131072', '-O', 'none,filetype,sparse_super,large_file',
        '-U', identifier, '-L', 'BRKDESKTOP', '-d', seed, image)
    inventory = normalize_owners(image, output)
    with image.open('rb') as stream:
        stream.seek(1024);sb = stream.read(1024)
    free_bytes = struct.unpack_from('<I', sb, 12)[0]*4096
    if free_bytes < 512*1024*1024:
        raise ValueError('Insufficient free space for package installation')
    with (output/'e2fsck.log').open('x') as log:
        run('e2fsck', '-fn', image, stdout=log, stderr=subprocess.STDOUT)
    result = {'format': 1, **identity, 'uuid': identifier, 'bytes': size,
              'sha256': digest(image), 'free_bytes': free_bytes,
              'packages': metadata['packages'], 'filesystem': inventory['source_features'],
              'namespace': inventory['summary'], 'writable_profile_directories': True,
              'home_path': '/nix/home/brook', 'home_uid': 1000,
              'physical_status': 'not provisioned; boot wiring and USB QEMU acceptance required'}
    (output/'manifest.json').write_text(json.dumps(result, indent=2)+'\n')
    remove_seed(seed)
    print('HARDWARE_WRITABLE_STORE_PREPARED', output, flush=True)


if __name__ == '__main__':
    main()
