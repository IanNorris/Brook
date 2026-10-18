#!/usr/bin/env python3
"""Check one-shot timerfd/epoll readiness and report user-observed latency.

Relative and absolute CLOCK_MONOTONIC timers use the same requested delay.
Timing includes scheduling/read overhead; it is not an interrupt-latency test.
"""
import argparse
import ctypes
import json
import os
import select
import statistics
import struct
import time


class Timespec(ctypes.Structure):
    _fields_ = [('seconds', ctypes.c_long), ('nanoseconds', ctypes.c_long)]


class Itimerspec(ctypes.Structure):
    _fields_ = [('interval', Timespec), ('value', Timespec)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--samples', type=int, default=64)
    parser.add_argument('--delay-ms', type=int, default=16)
    parser.add_argument('--mode', choices=['both', 'relative', 'absolute'], default='both')
    args = parser.parse_args()
    if args.samples < 1 or args.delay_ms < 1:
        parser.error('sample count and delay must be positive')
    libc = ctypes.CDLL(None, use_errno=True)
    libc.timerfd_create.argtypes = [ctypes.c_int, ctypes.c_int]
    libc.timerfd_create.restype = ctypes.c_int
    libc.timerfd_settime.argtypes = [ctypes.c_int, ctypes.c_int,
                                   ctypes.POINTER(Itimerspec), ctypes.c_void_p]
    libc.timerfd_settime.restype = ctypes.c_int
    fd = libc.timerfd_create(1, os.O_CLOEXEC | os.O_NONBLOCK)
    if fd < 0:
        raise OSError(ctypes.get_errno(), 'timerfd_create')
    try:
        with select.epoll() as epoll:
            epoll.register(fd, select.EPOLLIN)
            rows = []
            delay = args.delay_ms * 1000000
            for mode, flags in [('relative', 0), ('absolute', 1)]:
                if args.mode != 'both' and args.mode != mode:
                    continue
                samples = []
                for index in range(args.samples):
                    started = time.monotonic_ns()
                    expiry = started + delay if flags else delay
                    timer = Itimerspec(Timespec(0, 0),
                                       Timespec(expiry // 1000000000, expiry % 1000000000))
                    if libc.timerfd_settime(fd, flags, ctypes.byref(timer), None) < 0:
                        raise OSError(ctypes.get_errno(), 'timerfd_settime')
                    events = epoll.poll(max(1, args.delay_ms / 1000 * 10))
                    assert events == [(fd, select.EPOLLIN)], {
                        'mode': mode, 'sample': index, 'events': events,
                        'elapsed_ms': (time.monotonic_ns() - started) / 1000000,
                    }
                    data = os.read(fd, 8)
                    assert len(data) == 8 and struct.unpack('=Q', data)[0] == 1, data
                    samples.append(time.monotonic_ns() - started)
                ordered = sorted(samples)
                row = {'mode': mode, 'samples': len(samples),
                             'requested_ms': args.delay_ms,
                             'median_ms': statistics.median(samples) / 1000000,
                             'p95_ms': ordered[(len(ordered) * 95 + 99) // 100 - 1] / 1000000,
                             'max_ms': max(samples) / 1000000,
                             'observed_ns': samples}
                rows.append(row)
                print(json.dumps({'completed_mode': row}), flush=True)
            print(json.dumps({'uid': os.geteuid(), 'clock': 'CLOCK_MONOTONIC',
                              'results': rows}), flush=True)
            print('TIMERFD_LATENCY_PASS', flush=True)
    finally:
        os.close(fd)


if __name__ == '__main__':
    main()
