#!/usr/bin/env python3
"""Exercise a standalone FAT checker on disposable images, never a device."""
import argparse
import hashlib
from pathlib import Path
import struct
import shutil
import subprocess
import tempfile

from hardware_recovery_fsck import validate_checker


def sha(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').digest()


def check(checker, image, expected, text=None):
    before = sha(image)
    result = subprocess.run([str(checker), '-n', '--variant=standard', str(image)], stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    if sha(image) != before:
        raise RuntimeError('read-only checker modified the test filesystem')
    if (result.returncode != expected or
        (text and text not in result.stdout.lower())):
        raise RuntimeError(f'unexpected checker result {result.returncode}: {result.stdout[-2000:]!r}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checker', required=True, type=Path)
    args = parser.parse_args()
    checker = args.checker.resolve(strict=True)
    checker_info=validate_checker(checker)
    print('FAT_CHECKER_SHA256',checker_info['sha256'],flush=True)
    with tempfile.TemporaryDirectory(prefix='brook-fat-checker-') as temp:
        directory = Path(temp)
        payload = directory / 'record'; payload.write_bytes(bytes(range(128)))
        for bits, mb in ((16, 16), (32, 64)):
            image = directory / f'fat{bits}.img'
            with image.open('wb') as stream:
                stream.truncate(mb * 1024 * 1024)
            subprocess.run(['mkfs.fat', '-F', str(bits), str(image)], check=True,
                           stdout=subprocess.DEVNULL)
            for name in ('GUARD0.BIN', 'THIRD.BIN'):
                subprocess.run(['mcopy', '-i', str(image), str(payload), '::/' + name], check=True)
            check(checker, image, 0)
            clean = directory / f'fat{bits}-clean.img';shutil.copyfile(image, clean)
            with image.open('r+b') as stream:
                boot = stream.read(512)
                sector_size = struct.unpack_from('<H', boot, 11)[0]
                reserved = struct.unpack_from('<H', boot, 14)[0]
                fat_count = boot[16]
                root_entries = struct.unpack_from('<H', boot, 17)[0]
                fat_sectors = struct.unpack_from('<H', boot, 22)[0] or struct.unpack_from('<I', boot, 36)[0]
                if sector_size != 512 or not reserved or fat_count != 2 or not fat_sectors:
                    raise RuntimeError('unexpected fixture FAT geometry')
                data_sector = reserved + fat_count * fat_sectors + (root_entries * 32 + 511) // 512
                root_sector = (reserved + fat_count * fat_sectors if bits == 16 else
                               data_sector + (struct.unpack_from('<I', boot, 44)[0] - 2) * boot[13])
                stream.seek(root_sector * 512)
                entries = bytearray(stream.read(512))
                first = next(i for i in range(0, len(entries), 32) if entries[i:i+11] == b'GUARD0  BIN')
                other = next(i for i in range(0, len(entries), 32) if entries[i:i+11] == b'THIRD   BIN')
                cluster = struct.unpack_from('<H',entries,first+26)[0] | (struct.unpack_from('<H',entries,first+20)[0]<<16)
                other_cluster = struct.unpack_from('<H',entries,other+26)[0] | (struct.unpack_from('<H',entries,other+20)[0]<<16)
                if cluster<2 or other_cluster<2 or cluster==other_cluster:
                    raise RuntimeError('fixture files did not start with distinct allocations')
                for offset in (20, 26):
                    entries[other+offset:other+offset+2] = entries[first+offset:first+offset+2]
                stream.seek(root_sector * 512); stream.write(entries)
            check(checker, image, 1, b'/guard0.bin  and\n/third.bin\n  share clusters')
            shutil.copyfile(clean,image)
            with image.open('r+b') as stream:
                # Deliberately diverge the two FAT copies on the guard's
                # allocated cluster. The checker must not admit the image.
                location=(reserved+fat_sectors)*512+cluster*(2 if bits==16 else 4)
                stream.seek(location); original=stream.read(1)
                if not original: raise RuntimeError('FAT copy outside fixture')
                stream.seek(location);stream.write(bytes([original[0]^1]))
            check(checker,image,1,b'fats differ')
            shutil.copyfile(clean,image)
            with image.open('r+b') as stream:stream.truncate(4096)
            check(checker,image,1,b'failed to read sector')
            print(f'FAT{bits}_CLEAN_CROSSLINK_MIRROR_TRUNCATION_PASS', flush=True)


if __name__ == '__main__':
    main()
