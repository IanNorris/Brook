#!/usr/bin/env python3
"""Create a fresh FAT source image without bridge credentials."""

import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile

KEY_NAMES = frozenset({'hardware-update-key', 'bridge0.bin', 'bridge1.bin', 'bridget.tmp'})
PROVISION_MARKER = '::/etc/bridge-key-provision'
KEY_TEXT = re.compile(rb'[0-9a-f]{32}')
KEY_SECTOR = re.compile(rb'[0-9a-f]{32}\n?\x00{32}')
RECORD_MAGIC = b'BRKKEY01'


def fat_entries(image):
    listing = subprocess.check_output(
        ['mdir', '-a', '-b', '-s', '-i', str(image), '::/'], encoding='utf-8')
    entries = []
    for entry in listing.splitlines():
        if not entry.startswith('::/'):
            raise ValueError('Unexpected FAT directory listing')
        components = entry[3:].rstrip('/').split('/')
        if any(not name or name in ('.', '..') or any(c in name for c in '\\:*?[]')
               for name in components):
            raise ValueError('Unexpected FAT pathname')
        entries.append(entry)
    return entries


def has_key_name(entry):
    return any(component.casefold() in KEY_NAMES for component in entry[3:].split('/'))


def require_keyless_image(image, *, allow_local_provision=False, forbidden_keys=()):
    entries = fat_entries(image)
    for entry in entries:
        if has_key_name(entry):
            raise ValueError('Boot image contains a bridge credential path')
        if not allow_local_provision and entry.rstrip('/').casefold() == PROVISION_MARKER:
            raise ValueError('Deployable boot image requests local credential provisioning')

    # A removed FAT file can still leave its data in unused clusters. Inspect
    # the full image, including slack and free sectors, without logging bytes.
    raw = Path(image).read_bytes()
    for key in forbidden_keys:
        if not isinstance(key, bytes) or not KEY_TEXT.fullmatch(key):
            raise ValueError('Invalid credential supplied for boot image inspection')
        if key in raw:
            raise ValueError('Boot image contains bytes matching a bridge credential')
    candidates = [found.group()[:32] for offset in range(0, len(raw), 512)
                  if (found := KEY_SECTOR.match(raw, offset))]
    if candidates:
        # A machine-id has the same text format. Permit its one allocated
        # copy only. The guarded verifier also pins it to the source value.
        machine_id = next((entry for entry in entries if entry.casefold() == '::/etc/machine-id'), None)
        identity = (subprocess.check_output(['mtype', '-i', image, machine_id])
                    if machine_id else b'')
        if not re.fullmatch(rb'[0-9a-f]{32}\n?', identity) or candidates != [identity[:32]]:
            raise ValueError('Boot image contains standalone bridge key material')

    position = raw.find(RECORD_MAGIC)
    while position >= 0:
        record = raw[position:position + 512]
        if (len(record) == 512 and int.from_bytes(record[8:16], 'little') and
                KEY_TEXT.fullmatch(record[48:80]) and int.from_bytes(record[80:84], 'little') <= 2):
            raise ValueError('Boot image contains protected bridge record material')
        position = raw.find(RECORD_MAGIC, position + len(RECORD_MAGIC))


def rebuild_keyless_image(source, output):
    """Copy permitted files to a new FAT image with zeroed unused sectors."""
    entries = [entry for entry in fat_entries(source)
               if not has_key_name(entry) and entry.rstrip('/').casefold() != PROVISION_MARKER]
    size = source.stat().st_size
    with tempfile.TemporaryDirectory(prefix='brook-keyless-', dir=output.parent) as temporary:
        work = Path(temporary)
        image = work / 'initrd.img'
        with image.open('xb') as stream:
            stream.truncate(size)
        subprocess.run(['mformat', '-i', image, '::'], check=True)
        asset = work / 'asset'
        for entry in entries:
            if entry.endswith('/'):
                subprocess.run(['mmd', '-i', image, entry.rstrip('/')], check=True)
                continue
            subprocess.run(['mcopy', '-m', '-i', source, entry, asset], check=True)
            subprocess.run(['mcopy', '-m', '-i', image, asset, entry], check=True)
            asset.unlink()

        actual = {entry.casefold() for entry in fat_entries(image)}
        if actual != {entry.casefold() for entry in entries}:
            raise ValueError('Keyless source inventory does not match')
        require_keyless_image(image)
        subprocess.run(['fsck.fat', '-n', image], check=True, stdout=subprocess.DEVNULL)
        output.hardlink_to(image)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--source-sha256', required=True)
    args = parser.parse_args()
    try:
        with args.source.open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != args.source_sha256:
                raise ValueError('Source image is not the expected historical asset')
        rebuild_keyless_image(args.source, args.output)
        with args.output.open('rb') as stream:
            print(hashlib.file_digest(stream, 'sha256').hexdigest())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
