#!/usr/bin/env python3
"""Cross-check live realtime APIs against one monotonic interval (Linux x86-64).

Run as an ordinary guest user. Parent waits for every worker; nonzero exit means
an API regressed, disagreed, or drifted beyond the sampled call interval plus
5 ms. Absolute CMOS accuracy and non-KVM frequency accuracy are not established.
"""
import argparse
import ctypes as c
import json
import os
import time


def worker(seconds, raw, coarse_time=False):
    libc = c.CDLL(None, use_errno=True)
    libc.syscall.restype = c.c_long
    class Stamp(c.Structure):
        _fields_ = [('seconds', c.c_long), ('fraction', c.c_long)]
    def clock(which):
        stamp = Stamp()
        result = (libc.syscall(228, which, c.byref(stamp)) if raw else
                  libc.clock_gettime(which, c.byref(stamp)))
        if result: raise OSError(c.get_errno(), 'clock_gettime')
        if not 0 <= stamp.fraction < 1_000_000_000: raise ValueError('nanoseconds')
        return stamp.seconds * 1_000_000_000 + stamp.fraction
    start = clock(1)
    initial_offset = None
    backs = [0, 0, 0]
    previous = [0, 0, 0]
    largest = [0, 0, 0]
    disagreements = samples = drift_failures = 0
    max_drift = 0
    while True:
        before = clock(1)
        first = clock(0)
        tv = Stamp()
        result = (libc.syscall(96, c.byref(tv), 0) if raw else
                  libc.gettimeofday(c.byref(tv), None))
        if result: raise OSError(c.get_errno(), 'gettimeofday')
        if not 0 <= tv.fraction < 1_000_000: raise ValueError('microseconds')
        micros = tv.seconds * 1_000_000_000 + tv.fraction * 1000
        whole = libc.syscall(201, 0)
        last = clock(0)
        after = clock(1)
        values = [after, last, micros]
        for index, value in enumerate(values):
            if value < previous[index]:
                backs[index] += 1
                largest[index] = max(largest[index], previous[index] - value)
            previous[index] = value
        if (last < first or not first - 999 <= micros <= last + 999 or
                not first // 1_000_000_000 - int(coarse_time) <= whole <= last // 1_000_000_000):
            disagreements += 1
            if disagreements <= 3:
                print(json.dumps({'disagreement': {'first': first, 'gettimeofday': micros,
                      'time_seconds': whole, 'last': last}}), flush=True)
        offset = first - before
        if initial_offset is None: initial_offset = offset
        drift = abs(offset - initial_offset)
        max_drift = max(max_drift, drift)
        if drift > (after - before) + 5_000_000: drift_failures += 1
        samples += 1
        if after - start >= seconds * 1_000_000_000: break
        time.sleep(.001)
    passed = not any(backs) and not disagreements and not drift_failures
    print(json.dumps({'pid': os.getpid(), 'raw_syscalls': raw, 'samples': samples,
          'backsteps_mono_real_gtod': backs, 'largest_back_ns': largest,
          'api_disagreements': disagreements, 'drift_failures': drift_failures,
          'max_sampled_offset_drift_ns': max_drift, 'passed': passed}), flush=True)
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=15, choices=range(1, 61))
    parser.add_argument('--workers', type=int, default=8, choices=range(1, 33))
    parser.add_argument('--coarse-time', action='store_true',
                        help='Allow Linux time(2) to lag by one whole second; Brook acceptance omits this')
    args = parser.parse_args()
    children = []
    for index in range(args.workers):
        pid = os.fork()
        if not pid:
            try: passed = worker(args.seconds, raw=bool(index % 2), coarse_time=args.coarse_time)
            except Exception as exc:
                print(json.dumps({'pid': os.getpid(), 'error': str(exc), 'passed': False}), flush=True)
                passed = False
            os._exit(0 if passed else 1)
        children.append(pid)
    statuses = [os.waitpid(pid, 0)[1] for pid in children]
    passed = all(os.WIFEXITED(s) and os.WEXITSTATUS(s) == 0 for s in statuses)
    print('CLOCK_CONSISTENCY_' + ('PASS' if passed else 'FAIL'), flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
