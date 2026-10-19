#!/usr/bin/env python3
"""Guest-only canonical capture format/target check; creates no visible window.

Use a nonexistent window to distinguish accepted source metadata (ENOENT) from
unsupported source metadata (EINVAL). Pair completion with host backing-counter
checks; this probe alone does not establish retirement or captured pixel values.
"""
import argparse
import ctypes
import errno
import fcntl
import json
import os
import struct


def ioctl(fd, number, payload):
    value = bytearray(payload)
    fcntl.ioctl(fd, (3 << 30) | (len(value) << 16) | (ord('d') << 8) | number,
                value, True)
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--brook-guest', action='store_true', required=True)
    parser.parse_args()
    libc = ctypes.CDLL(None, use_errno=True)
    libc.syscall.restype = ctypes.c_long
    cases = {}
    for name, target, fmt, expected in (
            ('argb', 2, 1, errno.ENOENT),
            ('xrgb', 2, 2, errno.ENOENT),
            ('float_texture', 2, 28, errno.EINVAL),
            ('byte_buffer', 0, 64, errno.EINVAL)):
        fd = os.open('/dev/dri/renderD128', os.O_RDWR)
        imported = -1
        prime = -1
        try:
            # R32_FLOAT and R8_UNORM are valid virgl resources, but not display
            # capture sources. Testing them must not submit a capture command.
            values = struct.unpack('<14I', ioctl(fd, 0x44, struct.pack(
                '<14I', target, fmt, 10 if target else 16,
                4 if target else 64, 4 if target else 1,
                1, 1, 0, 0, 0, 0, 0, 64, 0)))
            handle, resource = values[10:12]
            assert handle and resource
            # Unexported resources never authorize numeric presentation.
            ctypes.set_errno(0)
            rc = libc.syscall(ctypes.c_long(525), ctypes.c_long(0xffffffff),
                               ctypes.c_long(resource), ctypes.c_long(4), ctypes.c_long(4))
            cases[name + '_unexported'] = rc == -1 and ctypes.get_errno() == errno.EINVAL
            prime = struct.unpack('<III', ioctl(fd, 0x2d,
                                  struct.pack('<III', handle, 3, 0)))[2]
            imported = os.open('/dev/dri/renderD128', os.O_RDWR)
            imported_handle = struct.unpack('<III', ioctl(imported, 0x2e,
                                           struct.pack('<III', 0, 0, prime)))[0]
            assert imported_handle
            os.close(fd)
            fd = -1
            os.close(prime)
            prime = -1
            # Only the imported GEM remains: format and target must follow the
            # canonical object after exporter/PRIME closure, not importer state.
            ctypes.set_errno(0)
            rc = libc.syscall(ctypes.c_long(525), ctypes.c_long(0xffffffff),
                               ctypes.c_long(resource), ctypes.c_long(4), ctypes.c_long(4))
            cases[name] = rc == -1 and ctypes.get_errno() == expected
        finally:
            if prime >= 0:
                os.close(prime)
            if imported >= 0:
                os.close(imported)
            if fd >= 0:
                os.close(fd)
    print('GPU_CAPTURE_FORMAT_RESULT', json.dumps({'pass': all(cases.values()),
          'cases': cases}), flush=True)
    return int(not all(cases.values()))


if __name__ == '__main__':
    raise SystemExit(main())
