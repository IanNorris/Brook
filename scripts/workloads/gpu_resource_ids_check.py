#!/usr/bin/env python3
"""Guest-only cumulative DRM resource creation and host roundtrip regression.

Creates and closes tiny private buffers through one render FD. No resources are
exported, no window is changed, and the command must drain before its endpoint
backing/retirement counters can be checked. This exercises resource creation;
the legacy upload entry point shares the allocator but is not exercised here.
"""
import argparse
import fcntl
import json
import mmap
import os
import struct
import time


def ioctl(fd, number, payload):
    data = bytearray(payload)
    fcntl.ioctl(fd, (3 << 30) | (len(data) << 16) | (ord('d') << 8) | number, data, True)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--brook-guest', required=True, action='store_true')
    parser.add_argument('--count', type=int, default=8192)
    args = parser.parse_args()
    if not 4097 <= args.count <= 32768:
        parser.error('--count must cross the old 4095-ID ceiling and stay bounded')
    started = time.monotonic()
    resources = set()
    roundtrips = []
    fd = os.open('/dev/dri/renderD128', os.O_RDWR)
    try:
        for i in range(args.count):
            values = struct.unpack('<14I', ioctl(fd, 0x44, struct.pack(
                '<14I', 0, 64, 1 << 4, 128, 1, 1, 1, 0, 0, 0, 0, 0, 128, 0)))
            handle, resource = values[10:12]
            assert handle and 0 < resource <= 0x7fffffff and resource not in resources
            resources.add(resource)
            try:
                if i in (0, 4094, 4095, 4096, args.count - 1):
                    offset = struct.unpack('<QII', ioctl(fd, 0x41, struct.pack('<QII', 0, handle, 0)))[0]
                    with mmap.mmap(fd, 4096, flags=mmap.MAP_SHARED,
                                   prot=mmap.PROT_READ | mmap.PROT_WRITE,
                                   offset=offset, trackfd=False) as memory:
                        pattern = bytes((i + n) % 256 for n in range(128))
                        memory[:128] = pattern
                        transfer = struct.pack('<11I', handle, 0, 0, 0, 128, 1, 1, 0, 0, 0, 0)
                        ioctl(fd, 0x47, transfer)
                        memory[:128] = bytes(128)
                        ioctl(fd, 0x46, transfer)
                        assert memory[:128] == pattern
                    roundtrips.append(i)
            finally:
                ioctl(fd, 0x09, struct.pack('<II', handle, 0))
    finally:
        os.close(fd)
    print('GPU_RESOURCE_IDS_RESULT', json.dumps({
        'pass': len(resources) == args.count,
        'created_and_closed': len(resources), 'unique_ids': len(resources),
        'first_resource': min(resources), 'last_resource': max(resources),
        'host_roundtrips_at_zero_based_indices': roundtrips,
        'seconds': time.monotonic() - started,
        'scope': 'One render context; private resources; no legacy upload or export retirement claim',
    }), flush=True)


if __name__ == '__main__':
    main()
