#!/usr/bin/env python3
"""Build the RTX desktop live-stick store: a raw ext2 /nix image for stick p2.

The profile is the VMware RC selection (same apps); the image is the same
format prepare_hardware_store.py builds for the VMware data partition. The
output directory gets STORE.IMG plus store-manifest.json with the exact byte
length, SHA-256 and a fresh GPT partition GUID; build_rtx_smoke_files.py
--live-store turns that into the initrd's /etc/live-store record, and
write-brook-rtx-store.sh writes STORE.IMG into p2 under that GUID.

No devices are touched. Run inside the pinned Nix shell.
"""
import argparse, hashlib, json, subprocess, sys, uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'scripts')]
DEFAULT_APPS = ('chromium', 'files', 'gallery', 'mousepad', 'qalculate')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--output', type=Path, required=True, help='new directory')
    p.add_argument('--free-mib', type=int, default=2048, help='ext2 headroom for RAM-only installs/home')
    p.add_argument('--selection-config', type=Path, help='profile selection JSON (default: VMware RC apps)')
    a = p.parse_args()
    from image_profile import closure_receipt, data_size_mib, evaluate, realize
    config = {'target': 'vmware', 'apps': list(DEFAULT_APPS), 'extraPackageNames': ['vim']}
    if a.selection_config:
        config = json.loads(a.selection_config.read_text())
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False, mode=0o700)
    selection = evaluate(config)
    profile = realize(config, selection)
    closure = closure_receipt(profile)
    mib = data_size_mib(closure['nar_bytes'], None, 1024, 32768, a.free_mib)
    selection_path = out / 'profile-selection.json'
    selection_path.write_text(json.dumps(selection, indent=2, sort_keys=True) + '\n')
    subprocess.run([sys.executable, str(ROOT / 'scripts/prepare_hardware_store.py'), '--profile', str(profile),
                    '--direct-ahci', '--size-mib', str(mib), '--output', str(out / 'profile'),
                    '--profile-selection', str(selection_path), '--auto-size'], check=True, cwd=ROOT)
    image = out / 'profile/STORE.IMG'
    size = image.stat().st_size
    if size % 4096:
        raise SystemExit(f'store size {size} is not a multiple of 4096')
    digest = hashlib.sha256()
    with image.open('rb') as f:
        for chunk in iter(lambda: f.read(1 << 22), b''):
            digest.update(chunk)
    part = uuid.uuid4()
    manifest = {
        'format': 'brook-rtx-live-store/1', 'image': str(image), 'bytes': size, 'sha256': digest.hexdigest(),
        'partition_guid': str(part), 'partition_guid_le_hex': part.bytes_le.hex(),
        'partition_name': 'BROOK_STORE', 'profile': str(profile), 'closure': closure, 'config': config,
        'record': f'guid={part.bytes_le.hex()} bytes={size} sha256={digest.hexdigest()}\n',
    }
    (out / 'store-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({k: manifest[k] for k in ('bytes', 'sha256', 'partition_guid')}, indent=2))


if __name__ == '__main__':
    main()
