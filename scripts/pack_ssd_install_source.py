#!/usr/bin/env python3
"""Pack a canonical Brook SSD image into bounded FAT-friendly segments.

Input must be a stopped, immutable regular-file disk image. This tool checks
GPT geometry and checksums, but does not authenticate the source, lock out
other writers, validate FAT/boot files, or authorize installation. Output is a
new directory of regular files; block-device sources are rejected.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import zlib

SECTOR = 512
ENTRY_BYTES = 128 * 128
ESP = bytes.fromhex('28732ac11ff8d211ba4b00a0c93ec93b')


def read_exact(fd, offset, length):
    data = os.pread(fd, length, offset)
    if len(data) != length:
        raise ValueError('short source read')
    return data


def checked_header(raw, sectors, backup):
    if raw[:8] != b'EFI PART':
        raise ValueError('missing GPT header')
    revision, size, checksum, reserved = struct.unpack_from('<IIII', raw, 8)
    if revision != 0x10000 or not 92 <= size <= SECTOR or reserved or any(raw[92:]):
        raise ValueError('unsupported GPT header')
    check = bytearray(raw[:size]); check[16:20] = bytes(4)
    if zlib.crc32(check) != checksum:
        raise ValueError('GPT header checksum')
    current, alternate, first, last = struct.unpack_from('<QQQQ', raw, 24)
    entries, count, entry_size, entries_crc = struct.unpack_from('<QIII', raw, 72)
    if (current, alternate, entries) != ((sectors - 1, 1, sectors - 33) if backup else (1, sectors - 1, 2)):
        raise ValueError('noncanonical GPT locations')
    if not 34 <= first <= last < sectors - 33 or (count, entry_size) != (128, 128) or not any(raw[56:72]):
        raise ValueError('unsupported GPT geometry')
    return first, last, raw[56:72], entries_crc


def inspect(fd, length):
    if length % SECTOR or not 68 <= length // SECTOR <= 1 << 48:
        raise ValueError('unsupported disk size')
    sectors = length // SECTOR
    primary = read_exact(fd, 0, 34 * SECTOR)
    backup = read_exact(fd, (sectors - 33) * SECTOR, 33 * SECTOR)
    mbr = primary[:SECTOR]
    entry = mbr[446:462]
    if (mbr[510:512] != b'\x55\xaa' or entry[0] != 0 or entry[4] != 0xee or
        struct.unpack_from('<II', entry, 8) != (1, min(sectors - 1, 0xffffffff)) or any(mbr[462:510])):
        raise ValueError('not a simple protective MBR')
    a = checked_header(primary[512:1024], sectors, False)
    b = checked_header(backup[-512:], sectors, True)
    entries, copy = primary[1024:], backup[:-512]
    if a != b or entries != copy or zlib.crc32(entries) != a[3]:
        raise ValueError('GPT copies or entry checksum differ')
    parts = [entries[i:i + 128] for i in range(0, ENTRY_BYTES, 128) if any(entries[i:i + 16])]
    if len(parts) != 1 or parts[0][:16] != ESP or not any(parts[0][16:32]):
        raise ValueError('expected one EFI partition')
    first, last = struct.unpack_from('<QQ', parts[0], 32)
    if not a[0] <= first <= last <= a[1]:
        raise ValueError('partition outside usable disk')
    return {'disk_sectors': sectors, 'payload_first': first, 'payload_sectors': last - first + 1}


def pack(source, output, segment_bytes):
    if segment_bytes < SECTOR or segment_bytes > 2 * 1024**3 or segment_bytes % SECTOR:
        raise ValueError('segment size must be sector aligned and at most 2 GiB')
    if not stat.S_ISREG(os.stat(source, follow_symlinks=False).st_mode):
        raise ValueError('source must be a regular file')
    fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode):
            raise ValueError('source must be a regular file')
        geometry = inspect(fd, before.st_size)
        sectors, first, count = (geometry[k] for k in ('disk_sectors', 'payload_first', 'payload_sectors'))
        spans = [(0, 34 * SECTOR), (first * SECTOR, count * SECTOR), ((sectors - 33) * SECTOR, 33 * SECTOR)]
        total = sum(length for _, length in spans)
        segment_count = (total + segment_bytes - 1) // segment_bytes
        if segment_count > 10000:
            raise ValueError('too many segments')
        output.mkdir(parents=True, exist_ok=False)
        digest = hashlib.sha256(); files = []; span_index = 0; within = 0
        for i in range(segment_count):
            size = min(segment_bytes, total - i * segment_bytes)
            name = f'INST{i:04}.IMG'; remaining = size; segment_hash = hashlib.sha256()
            with (output / name).open('xb') as stream:
                while remaining:
                    start, length = spans[span_index]
                    n = min(1024 * 1024, remaining, length - within)
                    data = read_exact(fd, start + within, n)
                    stream.write(data); digest.update(data); segment_hash.update(data)
                    remaining -= n; within += n
                    if within == length:
                        span_index += 1; within = 0
                stream.flush(); os.fsync(stream.fileno())
            files.append({'name': name, 'bytes': size, 'sha256': segment_hash.hexdigest()})
        after = os.fstat(fd)
        if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise ValueError('source changed during packing; discard output')
        manifest = {'format': 'brook-ssd-compact-v1', 'geometry': geometry, 'sector_bytes': SECTOR,
                    'segment_bytes': segment_bytes, 'packed_bytes': total, 'packed_sha256': digest.hexdigest(), 'segments': files}
        # This checksum binds the documented geometry and individual segment
        # hashes; it detects change but is not a signature or trusted approval.
        canonical = json.dumps(manifest, sort_keys=True, separators=(',', ':')).encode()
        manifest['manifest_sha256'] = hashlib.sha256(canonical).hexdigest()
        with (output / 'manifest.json').open('x') as stream:
            json.dump(manifest, stream, indent=2, sort_keys=True); stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
        directory_fd = os.open(output, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
        return manifest
    finally:
        os.close(fd)


def verify_segment(path, item, whole=None):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        st = os.fstat(fd); size = item['bytes']
        if not stat.S_ISREG(st.st_mode) or st.st_size != size:
            raise ValueError('invalid segment file')
        digest = hashlib.sha256(); pos = 0
        while pos < size:
            data = read_exact(fd, pos, min(1024 * 1024, size - pos))
            digest.update(data)
            if whole is not None: whole.update(data)
            pos += len(data)
        if digest.hexdigest() != item['sha256']:
            raise ValueError('segment checksum mismatch')
    finally:
        os.close(fd)


def verify_package(directory):
    # Checksums detect accidental change; the caller still needs a trusted
    # source of expected geometry and package identity before installation.
    manifest_path = directory / 'manifest.json'
    fd = os.open(manifest_path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode) or st.st_size > 1024 * 1024:
            raise ValueError('invalid manifest file')
        manifest = json.loads(read_exact(fd, 0, st.st_size))
    finally:
        os.close(fd)
    claimed = manifest.pop('manifest_sha256')
    canonical = json.dumps(manifest, sort_keys=True, separators=(',', ':')).encode()
    if hashlib.sha256(canonical).hexdigest() != claimed or manifest['format'] != 'brook-ssd-compact-v1':
        raise ValueError('manifest checksum or format')
    g = manifest['geometry']; sectors = g['disk_sectors']; first = g['payload_first']; count = g['payload_sectors']
    if not all(type(n) is int for n in (sectors, first, count)) or not 68 <= sectors <= 1 << 48 or not 34 <= first < sectors - 33 or not 0 < count <= sectors - 33 - first:
        raise ValueError('invalid package geometry')
    total = (34 + count + 33) * SECTOR; segment = manifest['segment_bytes']; files = manifest['segments']
    if (type(segment) is not int or not SECTOR <= segment <= 2 * 1024**3 or segment % SECTOR or
        manifest['sector_bytes'] != SECTOR or manifest['packed_bytes'] != total or
        not isinstance(files, list) or len(files) != (total + segment - 1) // segment or len(files) > 10000):
        raise ValueError('invalid package segmentation')
    whole = hashlib.sha256()
    for i, item in enumerate(files):
        name = f'INST{i:04}.IMG'; size = min(segment, total - i * segment)
        if item['name'] != name or item['bytes'] != size:
            raise ValueError('invalid segment description')
        verify_segment(directory / name, item, whole)
    if whole.hexdigest() != manifest['packed_sha256']:
        raise ValueError('packed checksum mismatch')
    manifest['manifest_sha256'] = claimed
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--segment-bytes', type=int, default=2 * 1024**3)
    args = parser.parse_args()
    result = pack(args.source, args.output, args.segment_bytes)
    print(f"PACKED {result['packed_bytes']} bytes in {len(result['segments'])} segments")


if __name__ == '__main__':
    main()
