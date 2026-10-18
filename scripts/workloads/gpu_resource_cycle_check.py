#!/usr/bin/env python3
"""Create/close tiny guest DRM buffers past the historical cumulative-ID limit.

Run only on isolated Brook GL disks. At most one 128-byte GEM is live at once;
4,500 iterations should not exhaust a budget for simultaneously live objects.
"""
import argparse
import fcntl
import json
import os
import struct


def ioctl(fd, number, payload):
    data = bytearray(payload)
    fcntl.ioctl(fd, (3 << 30) | (len(data) << 16) | (ord('d') << 8) | number, data, True)
    return data


def create(fd):
    return struct.unpack('<14I', ioctl(fd, 0x44, struct.pack(
        '<14I', 0, 64, 1 << 4, 128, 1, 1, 1, 0, 0, 0, 0, 0, 128, 0)))[10:12]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--brook-guest', action='store_true', required=True)
    p.add_argument('--allocations', type=int, default=4500)
    args = p.parse_args()
    if not 1 <= args.allocations <= 16000:
        p.error('allocations must be between 1 and 16000')
    fd = os.open('/dev/dri/renderD128', os.O_RDWR)
    completed = 0
    failure = None
    try:
        for i in range(args.allocations):
            try:
                handle, resource = create(fd)
            except OSError as e:
                failure = {'allocation_index': i, 'errno': e.errno, 'error': str(e)}
                break
            ioctl(fd, 0x09, struct.pack('<II', handle, 0))
            completed += 1
            if completed % 512 == 0:
                print('GPU_RESOURCE_CYCLE_PROGRESS', completed, resource, flush=True)
    finally:
        os.close(fd)
    # A new DRM context must also be able to allocate after the old one closes.
    reopened = False
    reopen_error = None
    fd = os.open('/dev/dri/renderD128', os.O_RDWR)
    try:
        try:
            handle, _ = create(fd)
            ioctl(fd, 0x09, struct.pack('<II', handle, 0))
            reopened = True
        except OSError as e:
            reopen_error = {'errno': e.errno, 'error': str(e)}
    finally:
        os.close(fd)
    result = {'requested': args.allocations, 'completed': completed,
              'max_live_gems': 1, 'failure': failure,
              'new_context_allocation_pass': reopened, 'new_context_error': reopen_error}
    print('GPU_RESOURCE_CYCLE_RESULT', json.dumps(result), flush=True)
    return int(failure is not None or not reopened)


if __name__ == '__main__':
    raise SystemExit(main())
