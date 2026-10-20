#!/usr/bin/env python3
"""Sample Linux x86-64 clock/sleep paths without opening audio or sending input.

Reports elapsed guest time, not a host-referenced frequency certification.
Run through a host-timestamped observer for that separate comparison.
"""
import ctypes as c
import errno
import json
import time


class Stamp(c.Structure):
    _fields_ = [('sec', c.c_long), ('nsec', c.c_long)]


def main():
    libc = c.CDLL(None, use_errno=True)
    libc.syscall.restype = c.c_long

    def raw_clock(clock):
        stamp = Stamp()
        if libc.syscall(c.c_long(228), c.c_long(clock), c.byref(stamp)):
            raise OSError(c.get_errno(), 'clock_gettime')
        return stamp.sec * 1_000_000_000 + stamp.nsec

    brackets = []
    for _ in range(32):
        before = raw_clock(1)
        fast = time.monotonic_ns()
        after = raw_clock(1)
        brackets.append(dict(before_ns=before, libc_ns=fast, after_ns=after,
                             within=before <= fast <= after))
    sleeps = []
    for mode in ('relative', 'absolute_monotonic', 'absolute_realtime'):
        for duration in (1_000_000, 10_000_000, 50_000_000):
            for _ in range(4):
                clock = 0 if mode == 'absolute_realtime' else 1
                started = raw_clock(1)
                target = duration if mode == 'relative' else raw_clock(clock) + duration
                request = Stamp(target // 1_000_000_000, target % 1_000_000_000)
                c.set_errno(0)
                if mode == 'relative':
                    result = libc.syscall(c.c_long(35), c.byref(request), c.c_void_p())
                else:
                    result = libc.syscall(c.c_long(230), c.c_long(clock), c.c_long(1),
                                          c.byref(request), c.c_void_p())
                error = c.get_errno() if result else 0
                ended = raw_clock(1)
                ended_selected = ended if clock == 1 else raw_clock(clock)
                early = result == 0 and (ended-started < duration if mode == 'relative'
                                         else ended_selected < target)
                sleeps.append(dict(mode=mode, requested_ns=duration, elapsed_ns=ended-started,
                                   result=result, errno=error,
                                   deadline_ns=None if mode == 'relative' else target,
                                   finished_clock_ns=ended_selected, early=early))
    passed = all(row['within'] for row in brackets) and all(
        not row['early'] and (row['result'] == 0 or row['errno'] == errno.EINTR)
        for row in sleeps)
    print(json.dumps(dict(passed=passed, clock_brackets=brackets, sleeps=sleeps)), flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
