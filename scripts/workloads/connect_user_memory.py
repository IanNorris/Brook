#!/usr/bin/env python3
"""Exercise connect's checked address copies through real Linux syscalls.

Covers pathname AF_UNIX and UDP AF_INET. The absent Unix destination may report
ENOENT or ECONNREFUSED; this case checks bounded copying, not socket namespace
conformance (see unix_path_socket_check.py).
"""
import ctypes
import errno
import json
import mmap
import socket
import struct


def main():
    libc = ctypes.CDLL(None, use_errno=True)
    libc.connect.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint]
    libc.connect.restype = ctypes.c_int
    libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
    page = mmap.PAGESIZE
    area = mmap.mmap(-1, page * 2, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS,
                     prot=mmap.PROT_READ | mmap.PROT_WRITE)
    base = ctypes.addressof(ctypes.c_char.from_buffer(area))
    assert libc.mprotect(base + page, page, 0) == 0
    cases = []
    def check(family, kind, name, address, length, allowed):
        with socket.socket(family, kind) as endpoint:
            ctypes.set_errno(0)
            rc = libc.connect(endpoint.fileno(), address, length)
            actual = ctypes.get_errno() if rc == -1 else 0
        cases.append(dict(family=family, name=name, result=rc, errno=actual,
                          expected_errno=list(allowed), passed=actual in allowed))
    try:
        for family, kind, size in [(socket.AF_UNIX, socket.SOCK_STREAM, 110),
                                    (socket.AF_INET, socket.SOCK_DGRAM, 16)]:
            ctypes.memmove(base + page - 2, struct.pack('H', family), 2)
            for name, address in [('null', 0), ('kernel_address', 0xffff800000000000),
                                  ('guard_page', base + page),
                                  ('cross_page_family', base + page - 2),
                                  ('cross_page_byte', base + page - 1)]:
                check(family, kind, name, address, size, [errno.EFAULT])
            ctypes.memmove(base, struct.pack('H', family), 2)
            for length in (0, 1, 2):
                check(family, kind, 'short_' + str(length), base, length, [errno.EINVAL])
            check(family, kind, 'oversized', base, 129, [errno.EINVAL])
        payload = struct.pack('H', socket.AF_UNIX) + b'/__brook_connect_absent_20261019\0'
        ctypes.memmove(base + page - len(payload), payload, len(payload))
        check(socket.AF_UNIX, socket.SOCK_STREAM, 'bounded_page_end',
              base + page - len(payload), len(payload), [errno.ENOENT, errno.ECONNREFUSED])
        payload = struct.pack('H', socket.AF_INET) + struct.pack('!H', 43219) + socket.inet_aton('127.0.0.1') + bytes(8)
        ctypes.memmove(base + page - len(payload), payload, len(payload))
        check(socket.AF_INET, socket.SOCK_DGRAM, 'bounded_page_end',
              base + page - len(payload), len(payload), [0])
        payload += bytes(112)
        ctypes.memmove(base + page - len(payload), payload, len(payload))
        check(socket.AF_INET, socket.SOCK_DGRAM, 'storage_page_end',
              base + page - len(payload), len(payload), [0])
    finally:
        assert libc.mprotect(base + page, page, mmap.PROT_READ | mmap.PROT_WRITE) == 0
        area.close()
    result = dict(pass_all=all(row['passed'] for row in cases), cases=cases,
                  scope='connect address-copy bounds; not full socket or entry-register coverage')
    print(json.dumps(result), flush=True)
    return 0 if result['pass_all'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
