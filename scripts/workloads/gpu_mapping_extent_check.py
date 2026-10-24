#!/usr/bin/env python3
"""Guest-only VirGL padded allocation and mmap reservation regression.

An oversized device mmap must fail rather than silently reserving a shorter
range: munmap uses the caller's original length and could remove a later VMA.
"""
import argparse
import ctypes as c
import errno
import fcntl
import json
import os
import struct


def ioctl(fd, number, payload):
    data = bytearray(payload)
    fcntl.ioctl(fd, (3 << 30) | (len(data) << 16) | (ord('d') << 8) | number, data, True)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--brook-guest', required=True, action='store_true')
    parser.parse_args()
    libc = c.CDLL(None, use_errno=True)
    libc.mmap.restype = c.c_void_p
    libc.mmap.argtypes = [c.c_void_p, c.c_size_t, c.c_int, c.c_int, c.c_int, c.c_long]
    libc.munmap.argtypes = [c.c_void_p, c.c_size_t]
    failed = c.c_void_p(-1).value
    fd = os.open('/dev/dri/renderD128', os.O_RDWR)
    cases = {}
    geometry = []
    try:
        for label, width, height, size, stride in (
                ('padded', 800, 600, 3328 * 600, 0),
                ('explicit_stride', 800, 600, 3328 * 600, 3328),
                ('placeholder', 64, 64, 1, 0)):
            values = struct.unpack('<14I', ioctl(fd, 0x44, struct.pack(
                '<14I', 2, 2, (1 << 1) | (1 << 3), width, height, 1, 1,
                0, 0, 0, 0, 0, size, stride)))
            handle = values[10]
            expected = max(size, (stride or width * 4) * height)
            extent = (expected + 4095) & ~4095
            address = following = failed
            try:
                cases[label + '_size'] = values[12] >= expected
                offset = struct.unpack('<QII', ioctl(fd, 0x41, struct.pack('<QII', 0, handle, 0)))[0]
                address = libc.mmap(None, extent, 3, 1, fd, offset)
                cases[label + '_map'] = address != failed
                if address != failed:
                    following = libc.mmap(None, 4096, 3, 0x22, -1, 0)
                    disjoint = following != failed and not (
                        address < following + 4096 and following < address + extent)
                    cases[label + '_reservation'] = disjoint
                    # Only touch the last byte once independent geometry proves
                    # the mapping did not silently swallow the following VMA.
                    if disjoint and cases[label + '_size']:
                        tail = c.c_ubyte.from_address(address + expected - 1)
                        tail.value = 0x5a
                        cases[label + '_tail'] = tail.value == 0x5a
                    geometry.append(dict(case=label, requested=extent, returned_size=values[12],
                                         address=address, following=following))
                if following != failed:
                    libc.munmap(following, 4096)
                    following = failed
                if address != failed:
                    libc.munmap(address, extent)
                    address = failed
                # No adjacent VMA exists during this intentionally invalid map.
                c.set_errno(0)
                oversized = libc.mmap(None, extent + 4096, 3, 1, fd, offset)
                error = c.get_errno()
                cases[label + '_oversize_rejected'] = oversized == failed and error == errno.EINVAL
                if oversized != failed:
                    libc.munmap(oversized, extent + 4096)
            finally:
                if following != failed:
                    libc.munmap(following, 4096)
                if address != failed:
                    libc.munmap(address, extent)
                ioctl(fd, 0x09, struct.pack('<II', handle, 0))
    finally:
        os.close(fd)
    print('GPU_MAPPING_EXTENT_RESULT', json.dumps(dict(
        passed=all(cases.values()), cases=cases, geometry=geometry)), flush=True)
    return int(not all(cases.values()))


if __name__ == '__main__':
    raise SystemExit(main())
