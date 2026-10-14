#!/usr/bin/env python3
"""Fetch the original DOOM 1.9 shareware fixture into ignored build assets."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parent.parent
URL = 'https://www.gamers.org/pub/idgames/idstuff/doom/doom19s.zip'
ZIP_SHA256 = 'cacf0142b31ca1af00796b4a0339e07992ac5f21bc3f81e7532fe1b5e1b486e6'
WAD_SHA256 = '1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771'


def validate(blob, expected):
    if hashlib.sha256(blob).hexdigest() != expected:
        raise ValueError('Fixture checksum mismatch; refusing unexpected game data')


def extract_wad(blob):
    validate(blob, ZIP_SHA256)
    # The DOS installer contains a split self-extracting ZIP. Read its data;
    # never execute DEICE.EXE or the self-extracting executable.
    with zipfile.ZipFile(io.BytesIO(blob)) as outer:
        joined = outer.read('DOOMS_19.1') + outer.read('DOOMS_19.2')
    with zipfile.ZipFile(io.BytesIO(joined)) as inner:
        wad = inner.read('DOOM1.WAD')
    validate(wad, WAD_SHA256)
    if wad[:4] != b'IWAD':
        raise ValueError('Fixture has no IWAD header')
    return wad


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'build/experiments/assets/doom/doom1.wad')
    args = parser.parse_args()
    target = args.output.resolve()
    # This helper deliberately cannot put copyrighted fixture data in source.
    if ROOT/'build' not in target.parents:
        parser.error('Output must be inside the ignored build directory')
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        validate(target.read_bytes(), WAD_SHA256)
        print(f'Already verified: {target}')
        return
    with urllib.request.urlopen(URL, timeout=30) as response:
        archive = response.read(8*1024*1024+1)
    wad = extract_wad(archive)
    with target.open('xb') as out:
        out.write(wad)
    (target.parent/'shareware-source.json').write_text(json.dumps({
        'url': URL, 'archive_sha256': ZIP_SHA256, 'wad_sha256': WAD_SHA256,
        'wad_bytes': len(wad), 'edition': 'DOOM 1.9 shareware'}, indent=2)+'\n')
    print(f'Verified DOOM shareware fixture: {target}')


if __name__ == '__main__':
    main()
