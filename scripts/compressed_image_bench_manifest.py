#!/usr/bin/env python3
"""Make a trusted x86-64 workload manifest for compressed_image_bench.c.

Enumerates each unique regular inode, hashes its verified decoded bytes and
records the exact guest path under --prefix. No image paths are accessed on the
host. Outputs are exclusive; a failed build may leave an incomplete manifest.
"""
import argparse
from pathlib import Path
import struct
from compressed_image import Image, FILE, CHUNK


def build(image_path, destination, prefix, library=None):
    if not prefix.startswith('/') or '\0' in prefix:
        raise ValueError('guest prefix must be an absolute non-NUL path')
    image = Image(image_path, library)
    try:
        selected, seen = [], set()
        for path, inode in image.paths():
            if image.nodes[inode].kind != FILE or inode in seen:
                continue
            seen.add(inode)
            guest = prefix.rstrip('/').encode() + b'/' + path
            if not 0 < len(guest) < 4096:
                raise ValueError('guest path exceeds workload limit')
            selected.append((guest, inode))
        if not 0 < len(selected) <= 65536:
            raise ValueError('workload requires 1..65536 unique regular files')
        with Path(destination).open('xb') as output:
            output.write(b'BRKBEN01' + struct.pack('<Q', len(selected)))
            for guest, inode in selected:
                size, digest = image.nodes[inode].size, 14695981039346656037
                for offset in range(0, size, CHUNK):
                    for byte in image.read(inode, offset, min(CHUNK, size-offset)):
                        digest = ((digest ^ byte) * 1099511628211) & ((1 << 64)-1)
                output.write(struct.pack('<IQQ', len(guest), size, digest) + guest)
        return len(selected)
    finally:
        image.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('--prefix', default='/nix/store')
    parser.add_argument('--lz4-library')
    args = parser.parse_args()
    print('manifest_files', build(args.image, args.manifest, args.prefix, args.lz4_library))


if __name__ == '__main__':
    main()
