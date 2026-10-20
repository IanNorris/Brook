#!/usr/bin/env python3
"""Read the calling thread's nice value through Linux syscall and libc APIs.

No priority writes, process selection changes or workload launches. Run this
same probe in Brook and a Linux reference. libc.syscall preserves successful
getpriority's positive kernel encoding; it translates syscall errors to errno.
"""
import argparse
import ctypes
import json
import os
import platform


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--expect-nice', type=int)
    args = parser.parse_args()
    if platform.machine() != 'x86_64':
        parser.error('This probe uses the x86-64 Linux syscall number')
    libc = ctypes.CDLL(None, use_errno=True)
    libc.syscall.restype = ctypes.c_long
    libc.getpriority.argtypes = [ctypes.c_int, ctypes.c_uint]
    libc.getpriority.restype = ctypes.c_int
    ctypes.set_errno(0)
    encoded = libc.syscall(ctypes.c_long(140), ctypes.c_long(0), ctypes.c_long(0))
    syscall_errno = ctypes.get_errno()
    ctypes.set_errno(0)
    native = libc.getpriority(0, 0)
    libc_errno = ctypes.get_errno()
    python = os.getpriority(os.PRIO_PROCESS, 0)
    valid = (not syscall_errno and not libc_errno and 1 <= encoded <= 40
             and -20 <= native <= 19 and native == python == 20 - encoded)
    expected = args.expect_nice is None or native == args.expect_nice
    print(json.dumps(dict(pid=os.getpid(), syscall_value=encoded,
                          syscall_errno=syscall_errno, libc_nice=native,
                          libc_errno=libc_errno, python_nice=python,
                          encoding_valid=valid, expected_nice=args.expect_nice,
                          expected_nice_matches=expected,
                          scope='Calling thread only; read-only; successful raw encoding via libc.syscall')),
          flush=True)
    return 0 if valid and expected else 1


if __name__ == '__main__':
    raise SystemExit(main())
