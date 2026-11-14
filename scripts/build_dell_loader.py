#!/usr/bin/env python3
"""Build the Dell XPS 13 9343 guarded EFI loader (BROOKGUARD.efi) reproducibly from this checkout.

Run inside the pinned dev shell from the repository root:

    nix-shell --run 'python3 scripts/build_dell_loader.py --output build/dell/loader-X'

The Dell-bound guard identity (disk GUID, partition GUID, ext2 UUID) and the data partition bounds
are private. They are read from <shared>/guard-loader-options.txt (mode 600, CMakeCache syntax:
BROOK_GUARD_IDENTITY:STRING=..., BROOK_GUARD_DATA_FIRST:STRING=..., BROOK_GUARD_DATA_SECTORS:STRING=...)
and passed to CMake through a private initial-cache file. They are never printed, and the manifest
records only a SHA-256 of the identity, so two builds can be compared without revealing it. The
output directory, which holds the CMake build tree, is created mode 700 because CMakeCache.txt and
compile_commands.json contain the identity; never commit or publish it.

Options match the installed v5 default (c717c048..., built 7 Nov from
build/hardware/dell-guard-v5-efi-candidate-20261107): BROOK_BUILD_GUARDED_BOOTLOADER,
BROOK_GUARD_ALTERNATE_RECOVERY and BROOK_GUARD_CHECK_DATA on; the inventory and probe builds off.

This only builds. Staging and promotion on the Dell are described in docs/dell-loader-promotion.md.
"""
import argparse, hashlib, json, os, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SHARED = Path('/data/Programming/Brook/shared/dell')
PUBLIC_OPTIONS = {
    'BROOK_BUILD_GUARDED_BOOTLOADER': 'ON', 'BROOK_GUARD_ALTERNATE_RECOVERY': 'ON',
    'BROOK_GUARD_CHECK_DATA': 'ON', 'BROOK_BUILD_BOOT_INVENTORY': 'OFF', 'BROOK_BUILD_RECOVERY_PROBE': 'OFF',
}
PRIVATE_KEYS = ('BROOK_GUARD_IDENTITY', 'BROOK_GUARD_DATA_FIRST', 'BROOK_GUARD_DATA_SECTORS')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_private(path):
    st = path.stat()
    if st.st_mode & 0o077:
        sys.exit(f'{path} must not be group/world accessible (chmod 600)')
    values = {}
    for line in path.read_text().splitlines():
        m = re.fullmatch(r'(BROOK_GUARD_[A-Z_]+):STRING=(\S+)', line.strip())
        if m:
            values[m.group(1)] = m.group(2)
    if set(values) != set(PRIVATE_KEYS):
        sys.exit(f'{path} must define exactly {", ".join(PRIVATE_KEYS)}')
    if not re.fullmatch(r'[0-9a-f]{96}', values['BROOK_GUARD_IDENTITY']):
        sys.exit('guard identity must be 96 lowercase hex digits')
    for key in PRIVATE_KEYS[1:]:
        if not re.fullmatch(r'[1-9][0-9]*', values[key]):
            sys.exit(f'{key} must be a positive decimal')
    return values


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--output', type=Path, required=True, help='new private directory (must not exist)')
    p.add_argument('--shared', type=Path, default=DEFAULT_SHARED)
    p.add_argument('--allow-dirty', action='store_true', help='permit uncommitted changes (recorded)')
    a = p.parse_args()
    out = a.output.resolve()
    if out.exists():
        sys.exit(f'{out} already exists; loader builds are never overwritten')
    commit = subprocess.run(['git', '-C', ROOT, 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
    dirty = bool(subprocess.run(['git', '-C', ROOT, 'status', '--porcelain', '--untracked-files=no'],
                                capture_output=True, text=True, check=True).stdout.strip())
    if dirty and not a.allow_dirty:
        sys.exit('Working tree has uncommitted changes; commit them or pass --allow-dirty')
    private = read_private(a.shared.resolve() / 'guard-loader-options.txt')

    if not (ROOT / 'vendor/uefi-headers/Include/Uefi.h').exists():
        subprocess.run(['git', '-C', ROOT, 'submodule', 'update', '--init', '--', 'vendor/uefi-headers'], check=True)

    out.mkdir(parents=True, mode=0o700)
    build = out / 'build'
    build.mkdir(mode=0o700)
    cache = out / 'private-initial-cache.cmake'
    fd = os.open(cache, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as f:
        for key, value in private.items():
            f.write(f'set({key} "{value}" CACHE STRING "")\n')
    configure = ['cmake', '-S', ROOT / 'src/bootloader', '-B', build, '-G', 'Ninja', '-C', cache,
                 f'-DCMAKE_TOOLCHAIN_FILE={ROOT}/cmake/toolchains/uefi-x86_64-clang.cmake']
    configure += [f'-D{k}:BOOL={v}' for k, v in sorted(PUBLIC_OPTIONS.items())]
    print('+ cmake configure (private guard values from initial cache)', flush=True)
    subprocess.run([str(x) for x in configure], check=True, stdout=subprocess.DEVNULL)
    print('+ cmake --build', build, flush=True)
    subprocess.run(['cmake', '--build', str(build), '--target', 'bootloader_guarded'], check=True)
    cache.unlink()  # the build tree's CMakeCache.txt still holds the values; the directory stays 700

    image = (build / 'BROOKGUARD.efi').read_bytes()
    if image[:2] != b'MZ':
        sys.exit('BROOKGUARD.efi is not a PE image')
    (out / 'BROOKGUARD.efi').write_bytes(image)
    manifest = {
        'commit': commit, 'dirty': dirty, 'sha256': sha(image), 'bytes': len(image),
        'options': PUBLIC_OPTIONS,
        'guard_identity_sha256': sha(private['BROOK_GUARD_IDENTITY'].encode()),
        'guard_data_bounds_sha256': sha(f"{private['BROOK_GUARD_DATA_FIRST']}:{private['BROOK_GUARD_DATA_SECTORS']}".encode()),
        'boot_protocol': next((line.split('=')[1].strip().rstrip(';') for line in
                               (ROOT / 'src/shared/boot_protocol/boot_protocol.h').read_text().splitlines()
                               if 'BootProtocolVersion =' in line), None),
    }
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({k: manifest[k] for k in ('commit', 'dirty', 'sha256', 'bytes', 'boot_protocol')}, indent=2))


if __name__ == '__main__':
    main()
