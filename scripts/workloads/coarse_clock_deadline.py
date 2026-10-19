#!/usr/bin/env python3
"""Check coarse clock epochs and a realtime absolute timerfd deadline."""
import ctypes
import json
import os
import select
import time


class Timespec(ctypes.Structure):
    _fields_ = [('seconds', ctypes.c_long), ('nanoseconds', ctypes.c_long)]


class Itimerspec(ctypes.Structure):
    _fields_ = [('interval', Timespec), ('value', Timespec)]


def main():
    libc = ctypes.CDLL(None, use_errno=True)
    libc.timerfd_create.argtypes = [ctypes.c_int, ctypes.c_int]
    libc.timerfd_settime.argtypes = [ctypes.c_int, ctypes.c_int,
                                   ctypes.POINTER(Itimerspec), ctypes.c_void_p]
    checks = {}
    readings = {}
    for label, normal, coarse in [('realtime', 0, 5), ('monotonic', 1, 6)]:
        before = time.clock_gettime_ns(normal)
        value = time.clock_gettime_ns(coarse)
        after = time.clock_gettime_ns(normal)
        resolution = time.clock_getres(coarse)
        readings[label] = dict(before=before, coarse=value, after=after,
                               resolution_seconds=resolution)
        checks[label + '-epoch'] = before - 100_000_000 <= value <= after + 100_000_000
        checks[label + '-resolution'] = 0 < resolution <= .001
    fd = libc.timerfd_create(0, os.O_NONBLOCK | os.O_CLOEXEC)
    assert fd >= 0, ctypes.get_errno()
    try:
        deadline = time.clock_gettime_ns(5) + 200_000_000
        timer = Itimerspec(Timespec(0, 0), Timespec(*divmod(deadline, 1_000_000_000)))
        start = time.monotonic()
        assert libc.timerfd_settime(fd, 1, ctypes.byref(timer), None) == 0, ctypes.get_errno()
        readable, _, _ = select.select([fd], [], [], 2)
        elapsed = time.monotonic() - start
        checks['absolute-deadline-ready'] = bool(readable)
        checks['absolute-deadline-waited'] = .1 <= elapsed < 2
        checks['one-expiration'] = bool(readable) and int.from_bytes(os.read(fd, 8), 'little') == 1
        passed = all(checks.values())
        print(json.dumps(dict(passed=passed, checks=checks, readings=readings,
                              elapsed_seconds=elapsed)))
        return 0 if passed else 1
    finally:
        os.close(fd)


if __name__ == '__main__':
    raise SystemExit(main())
