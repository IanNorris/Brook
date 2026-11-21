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

from collect_desktop_entries import collect
from selected_launchers import collect_selected

ROOT = Path(__file__).resolve().parents[1]
MIB = 1024 * 1024
FAT_FILE_MAX = 0xfffffe00
MIN_FREE = 512 * MIB
INTEL_VULKAN_DIR = 'system/graphics/intel-vulkan-26.0.6-brook1'
STAGE_GENERATION = '/var/.brook-profiles/.brook-profile-legacy'


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


def seed_intel_vulkan(seed, driver, closure):
    """Install the trusted, self-contained ICD used by the desktop environment."""
    driver = Path(driver)
    if driver.parent != Path('/nix/store') or str(driver) not in closure:
        raise ValueError('Intel Vulkan driver must belong to the profile closure')
    source_lib = driver/'lib/libvulkan_intel_hasvk.so'
    source_manifest = driver/'share/vulkan/icd.d/intel_hasvk_icd.x86_64.json'
    manifest = json.loads(source_manifest.read_text())
    if manifest['ICD']['library_path'] != str(source_lib):
        raise ValueError('Unexpected Intel Vulkan ICD library path')
    destination = seed/INTEL_VULKAN_DIR
    destination.mkdir(parents=True, mode=0o755)
    for directory in (seed/'system', seed/'system/graphics', destination):
        directory.chmod(0o755)
    library = destination/'libvulkan_intel_hasvk.so'
    shutil.copyfile(source_lib, library)
    library.chmod(0o644)
    manifest['ICD']['library_path'] = '/nix/'+INTEL_VULKAN_DIR+'/libvulkan_intel_hasvk.so'
    target_manifest = destination/'intel_hasvk.json'
    target_manifest.write_text(json.dumps(manifest, indent=2)+'\n')
    target_manifest.chmod(0o644)
    return {'driver': str(driver), 'library_sha256': digest(library),
            'manifest_sha256': digest(target_manifest)}


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


def free_space(image):
    with image.open('rb') as stream:
        stream.seek(1024)
        return struct.unpack_from('<I', stream.read(1024), 12)[0] * 4096


def tree_entries(root):
    return 1 + sum(len(dirs) + len(files)
                   for _, dirs, files in os.walk(root, followlinks=False))


def profile_generation_footprint(image):
    source = Source(image)
    try:
        nodes = source.inventory(STAGE_GENERATION)['nodes']
        inodes = {meta['ino'] for meta in nodes.values()}
        # ext2 i_blocks counts 512-byte sectors, including indirect blocks.
        sectors = sum(struct.unpack_from('<I', source.inode(ino)[1], 28)[0] for ino in inodes)
        return {'inodes': len(inodes), 'bytes': sectors * 512}
    finally:
        source.close()


def free_inodes(image):
    with image.open('rb') as stream:
        stream.seek(1024 + 16)
        return struct.unpack('<I', stream.read(4))[0]


def grow_selected_store(image, size, limit, auto, required_free=MIN_FREE):
    free = free_space(image)
    required_mib = (required_free + MIB - 1) // MIB
    while free < required_free:
        minimum = (size + required_free - free + MIB - 1) // MIB
        if not auto or minimum * MIB > limit:
            raise ValueError(f'Selected store needs at least {minimum} MiB to leave {required_mib} MiB '
                             'for staging and package installs')
        # Growth itself allocates more group metadata. Try with a little
        # additional room before normalizing ownership on the final image.
        grown = min((minimum + 8) * MIB, limit // MIB * MIB)
        if grown <= size:
            raise ValueError(f'Selected store cannot leave {required_mib} MiB free within {limit // MIB} MiB')
        with image.open('r+b') as stream:
            stream.truncate(grown)
        run('resize2fs', image)
        size = grown
        free = free_space(image)
    return size


def format_store(image, size, seed, identifier, inodes, limit, auto):
    args = ['mke2fs', '-q', '-t', 'ext2', '-b', '4096', '-I', '128', '-m', '0',
            '-N', str(inodes), '-O', 'none,filetype,sparse_super,large_file',
            '-U', identifier, '-L', 'BRKDESKTOP', '-d', seed, image]
    while True:
        with image.open('wb') as stream:
            stream.truncate(size)
        if not auto:
            run(*args)
            return size
        result = subprocess.run(list(map(str, args)), cwd=ROOT, text=True, capture_output=True,
                                env=dict(os.environ, LC_ALL='C'))
        if result.returncode == 0:
            if result.stderr:
                print(result.stderr, end='')
            return size
        # mke2fs can run out of space before writing a complete filesystem.
        # Retry only that condition, on our own private image file.
        grown = min(size + max(size // (4 * MIB), 128) * MIB, limit // MIB * MIB)
        if 'Could not allocate block' not in result.stderr or grown <= size:
            raise ValueError(f'Could not format selected store at {size // MIB} MiB: {result.stderr.strip()}')
        size = grown


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path)
    source = parser.add_mutually_exclusive_group()
    source.add_argument('--expression', type=Path, help='Profile package expression (default: hardware network)')
    source.add_argument('--profile-selection', type=Path, help='Metadata for the realized selected-app profile')
    parser.add_argument('--index', type=Path, default=ROOT/'tools/nix-index/packages.idx')
    parser.add_argument('--size-mib', type=int, default=3840)
    parser.add_argument('--auto-size', action='store_true',
                        help='Allow a selected profile to grow beyond its estimated size')
    parser.add_argument('--direct-ahci', action='store_true',
                        help='Allow a larger image for a direct partition, outside a FAT32 file')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.auto_size and not args.profile_selection:
        parser.error('--auto-size requires --profile-selection')
    selection = None
    if args.profile_selection:
        from image_profile import load_selection, verify_realized_selection
        selection = load_selection(args.profile_selection, args.profile)
        profile = verify_realized_selection(selection)
    elif args.profile:
        profile = args.profile.resolve(strict=True)
    else:
        parser.error('Specify --profile or --profile-selection')
    index = args.index.resolve(strict=True)
    index_metadata = json.loads(Path(str(index)+'.json').read_text())
    if (index_metadata.get('format') != 1 or
        index_metadata.get('index_sha256') != digest(index) or
        index_metadata.get('nixpkgs_import_sha256') != digest(ROOT/'nix/nixpkgs.nix') or
        index_metadata.get('system') != 'x86_64-linux' or
        index_metadata.get('allow_unfree') is not False):
        parser.error('Index provenance must match Brook\'s pinned x86_64-linux free package set; regenerate with tools/nix-index/gen-nix-index.py')
    size = args.size_mib * MIB
    if profile.parent != Path('/nix/store') or not profile.is_dir():
        parser.error('Profile must resolve to a built Nix store directory')
    limit = 32 * 1024 * 1024 * 1024 if args.direct_ahci else FAT_FILE_MAX
    if not MIN_FREE <= size <= limit:
        parser.error('Image size is out of range for the selected disk layout')
    compiler = os.environ.get('MUSL_CC')
    if not compiler:
        parser.error('Run inside nix-shell with MUSL_CC')
    output = args.output.absolute()
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    closure = subprocess.check_output(['nix-store', '-qR', str(profile)], text=True).splitlines()
    if not closure or any(Path(p).parent != Path('/nix/store') for p in closure):
        raise ValueError('Invalid Nix closure')
    if selection:
        metadata = selection
        if any(str(Path('/nix/store') / row['store']) not in closure for row in metadata['packages']):
            raise ValueError('Selected package metadata refers outside the profile closure')
    else:
        expression = (args.expression or ROOT/'nix/hardware-network-packages.nix').resolve(strict=True)
        nix = ('let e = import (builtins.toPath '+json.dumps(str(expression))+') {}; in '
               '{ profile = toString e.profile; packages = map (p: { '
               'name = p.pname or p.name; version = p.version or ""; '
               'store = builtins.baseNameOf (toString p); }) e.packages; '
               'intelVulkanDriver = if e ? intelVulkanDriver then toString e.intelVulkanDriver else null; }')
        metadata = json.loads(subprocess.check_output(
            ['nix-instantiate', '--eval', '--strict', '--json', '-E', nix], text=True))
    if metadata['profile'] != str(profile):
        raise ValueError('Expression and profile do not match')
    (output/'closure.txt').write_text('\n'.join(sorted(closure))+'\n')
    seed = output/'seed';(seed/'store').mkdir(parents=True)
    for item in closure:
        run('cp', '-a', '--reflink=auto', '--', item, seed/'store'/Path(item).name)
    intel_vulkan = (seed_intel_vulkan(seed, metadata['intelVulkanDriver'], closure)
                    if metadata['intelVulkanDriver'] else None)
    profiles = seed/'var/.brook-profiles'
    profiles.mkdir(parents=True)
    profile_tree(profile, profiles/'.brook-profile-legacy', {Path(p).name for p in closure})
    (profiles/'current').symlink_to('.brook-profile-legacy')
    (profiles/'store').symlink_to('../../store')
    (seed/'profile').symlink_to('var/.brook-profiles/current')
    (seed/'share').mkdir()
    (seed/'share/applications').symlink_to('../profile/.brook-launcher')
    (seed/'profile/.brook-launcher').mkdir()
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
    stage_profile = profile/'bin/brook-stage-profile'
    if stage_profile.is_file():
        (seed/'bin/brook-stage-profile').symlink_to(stage_profile.resolve())
    for name in ('curl', 'xz', 'zstd'):
        if not (profile/'bin'/name).is_file():
            raise ValueError('Required runtime helper missing: '+name)
        (seed/'bin'/name).symlink_to('../profile/bin/'+name)
    ca = profile/'etc/ssl/certs/ca-bundle.crt'
    if not ca.is_file():
        raise ValueError('Profile CA bundle missing')
    (seed/'etc/ssl/certs/ca-bundle.crt').symlink_to('/nix/profile/etc/ssl/certs/ca-bundle.crt')
    shutil.copyfile(index, seed/'index/packages.idx')
    (seed/'index/packages.idx.json').write_text(json.dumps(index_metadata, indent=2)+'\n')
    rows = []
    for p in metadata['packages']:
        fields = [p['name'], p['version'], p['store']]
        if any('\n' in f or '\t' in f for f in fields):
            raise ValueError('Invalid package manifest field')
        rows.append('\t'.join(fields))
    (seed/'profile/manifest.tsv').write_text('\n'.join(sorted(set(rows)))+'\n')
    # collect_desktop_entries resolves guest /nix paths against the seed and
    # uses only the profile exports, avoiding uninstalled closure-only entries.
    if selection:
        collect_selected(seed/'store', seed/'share/applications', profile,
                         selection['manifest']['launcherIds'], hardware=True)
    else:
        collect(seed/'store', seed/'share/applications', hardware=True)
    (seed/'BROOK.MNT').write_text('/nix\n')
    identity = {'profile': str(profile), 'closure': sorted(closure),
                'index_sha256': digest(index), 'index_metadata': index_metadata, 'tools': tools,
                'intel_vulkan': intel_vulkan,
                'builder_sha256': digest(Path(__file__))}
    if selection:
        identity['profile_selection'] = selection
    identifier = str(uuid.uuid5(uuid.NAMESPACE_URL, json.dumps(identity, sort_keys=True)))
    (seed/'.brook-store.json').write_text(json.dumps({'format': 1, 'uuid': identifier, **identity}, indent=2)+'\n')
    image = output/'STORE.IMG'
    image.touch(exist_ok=False)
    inodes = 131072
    if args.direct_ahci or selection:
        # The desktop closure can have hundreds of thousands of small files
        # and symlinks. Leave room for later Gallery installs as well.
        entries = tree_entries(seed)
        reserve = tree_entries(seed/STAGE_GENERATION.lstrip('/')) if selection else 0
        inodes = max(size // 32768, entries + max(32768, entries // 8) + reserve)
    size = format_store(image, size, seed, identifier, inodes, limit, args.auto_size)
    generation_reserve = None
    if selection:
        # A stage creates a complete generation before replacing the alias.
        # Reserve its observed on-disk allocation as well as package headroom.
        generation_reserve = profile_generation_footprint(image)
        size = grow_selected_store(image, size, limit, args.auto_size,
                                   MIN_FREE + generation_reserve['bytes'])
        if free_inodes(image) < generation_reserve['inodes']:
            raise ValueError(f"Selected store needs at least {generation_reserve['inodes']} free inodes "
                             "to stage another profile generation")
    inventory = normalize_owners(image, output)
    free_bytes = free_space(image)
    if free_bytes < MIN_FREE:
        raise ValueError('Insufficient free space for package installation')
    with (output/'e2fsck.log').open('x') as log:
        run('e2fsck', '-fn', image, stdout=log, stderr=subprocess.STDOUT)
    result = {'format': 1, **identity, 'uuid': identifier, 'bytes': size,
              'sha256': digest(image), 'free_bytes': free_bytes,
              'packages': metadata['packages'], 'filesystem': inventory['source_features'],
              'namespace': inventory['summary'], 'writable_profile_directories': True,
              'home_path': '/nix/home/brook', 'home_uid': 1000,
              'physical_status': 'not provisioned; boot wiring and USB QEMU acceptance required'}
    if generation_reserve is not None:
        result['stage_generation_reserve'] = generation_reserve
        result['free_inodes'] = free_inodes(image)
    (output/'manifest.json').write_text(json.dumps(result, indent=2)+'\n')
    remove_seed(seed)
    print('HARDWARE_WRITABLE_STORE_PREPARED', output, flush=True)


if __name__ == '__main__':
    main()
