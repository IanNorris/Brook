#!/usr/bin/env python3
"""Bounded x86-64 futex timeout, validation and concurrent real-wake probes.

Selected-clock samples after the syscall conservatively detect early timeout
returns. Spurious successful wakes are counted separately, not called timeout
failures. A separate four-lane probe exercises bounded real-wake races.
This measures the API, not CPU-service fairness or an exhaustive SMP proof.
"""
import argparse
import ctypes as c
import errno
import json
import platform
import time
import threading


class Stamp(c.Structure):
    _fields_ = [('sec', c.c_long), ('nsec', c.c_long)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--samples', type=int, default=12, choices=range(1, 33))
    args = parser.parse_args()
    if platform.machine().lower() not in ('x86_64', 'amd64'):
        raise SystemExit('This probe uses Linux x86-64 syscall numbers')
    libc = c.CDLL(None, use_errno=True)
    libc.syscall.restype = c.c_long
    word = c.c_uint32(0)

    def clock(clockid=1):
        stamp = Stamp()
        if libc.syscall(c.c_long(228), c.c_long(clockid), c.byref(stamp)):
            raise OSError(c.get_errno(), 'clock_gettime')
        return stamp.sec * 1_000_000_000 + stamp.nsec

    def wait(op, request):
        c.set_errno(0)
        result = libc.syscall(c.c_long(202), c.byref(word), c.c_long(op), c.c_long(0),
                              request, c.c_void_p(), c.c_long(0xffffffff))
        return result, c.get_errno() if result == -1 else 0

    groups = []
    for mode in ('relative', 'absolute_monotonic', 'absolute_realtime'):
        for duration in (100_000, 500_000, 1_000_000, 2_000_000, 5_000_000, 10_000_000):
            elapsed, errors, examples = [], {}, []
            early = backwards = spurious = unexpected = 0
            for index in range(args.samples):
                # Spread entry phase without an unbounded spin or catch-up loop.
                if index % 5:
                    time.sleep((index % 5) * .00017)
                selected = 0 if mode == 'absolute_realtime' else 1
                before = clock()
                start = clock(0) if selected == 0 else before
                target = start + duration
                value = duration if mode == 'relative' else target
                request = Stamp(value // 1_000_000_000, value % 1_000_000_000)
                op = 128 if mode == 'relative' else 9 | 128 | (256 if selected == 0 else 0)
                result, error = wait(op, c.byref(request))
                finished_selected = clock(selected)
                after = clock() if selected == 0 else finished_selected
                delta = after - before
                elapsed.append(delta)
                errors[str(error)] = errors.get(str(error), 0) + 1
                spurious += result == 0
                unexpected += result not in (-1,0)
                backwards += delta < 0
                is_early = error == errno.ETIMEDOUT and finished_selected < target
                early += is_early
                if is_early and len(examples) < 1:
                    examples.append(dict(before_ns=before, selected_start_ns=start,
                                         target_ns=target, finished_selected_ns=finished_selected,
                                         elapsed_ns=delta, errno=error))
            ordered = sorted(elapsed)
            groups.append(dict(mode=mode, requested_ns=duration, samples=args.samples,
                               minimum_ns=ordered[0], median_ns=ordered[len(ordered)//2],
                               maximum_ns=ordered[-1], early_timeouts=early,
                               backwards=backwards, spurious_successes=spurious, unexpected_returns=unexpected,
                               errno_counts=errors, early_examples=examples))
    invalid = []
    for name, request, expected in (
            ('negative_seconds', c.byref(Stamp(-1, 0)), errno.EINVAL),
            ('nanosecond_overflow', c.byref(Stamp(0, 1_000_000_000)), errno.EINVAL),
            ('unreadable_timeout', c.c_void_p(1), errno.EFAULT)):
        result, error = wait(128, request)
        invalid.append(dict(name=name, result=result, errno=error, expected_errno=expected,
                            passed=result == -1 and error == expected))
    # Four independent address pairs exercise real wake-before-block races,
    # delayed wakeups and waiter teardown. Every syscall has a finite bound;
    # no failed wait can strand a thread indefinitely.
    lanes = []
    def wake_lane(lane):
        cell = c.c_uint32(0)
        failures = []
        counts = {}
        for index in range(24):
            cell.value = 0
            receipt = {}
            duration = 1_000_000 if index % 2 else 100_000_000
            def sleeper():
                request = Stamp(0, duration)
                started = clock()
                c.set_errno(0)
                result = libc.syscall(c.c_long(202), c.byref(cell), c.c_long(128),
                    c.c_long(0), c.byref(request), c.c_void_p(), c.c_long(0))
                receipt.update(result=result, errno=c.get_errno() if result == -1 else 0,
                               start_ns=started, end_ns=clock())
            thread = threading.Thread(target=sleeper, daemon=True)
            thread.start()
            time.sleep((0, .0001, .00075, .0011)[index % 4])
            cell.value = 1
            wake_started = clock()
            woke = libc.syscall(c.c_long(202), c.byref(cell), c.c_long(129),
                                c.c_long(1), c.c_void_p(), c.c_void_p(), c.c_long(0))
            thread.join(2)
            if thread.is_alive():
                failures.append(dict(round=index, reason='bounded wait did not finish'))
                break
            error = receipt['errno']
            counts[str(error)] = counts.get(str(error), 0) + 1
            if woke not in (0, 1) or error not in (0, errno.EAGAIN, errno.EINTR, errno.ETIMEDOUT):
                failures.append(dict(round=index, reason='unexpected wake/wait result',
                                     wake_result=woke, **receipt))
            if error == errno.ETIMEDOUT:
                if receipt['end_ns'] - receipt['start_ns'] < duration:
                    failures.append(dict(round=index, reason='early timeout', **receipt))
                elif woke == 1 and wake_started - receipt['start_ns'] < duration // 2:
                    failures.append(dict(round=index, reason='timely real wake lost', **receipt))
        lanes.append(dict(lane=lane, rounds=sum(counts.values()), errno_counts=counts,
                          failures=failures, passed=not failures and sum(counts.values()) == 24))
    workers = [threading.Thread(target=wake_lane, args=(i,), daemon=True) for i in range(4)]
    for worker in workers: worker.start()
    for worker in workers: worker.join(12)
    wake_races = dict(passed=len(lanes) == 4 and all(v['passed'] for v in lanes),
                      expected_rounds=96, lanes=sorted(lanes, key=lambda v: v['lane']))
    passed = (all(not v['early_timeouts'] and not v['backwards'] and not v['unexpected_returns'] and
                  set(v['errno_counts']) <= {str(errno.ETIMEDOUT), str(errno.EINTR), '0'}
                  for v in groups) and all(v['passed'] for v in invalid) and wake_races['passed'])
    print(json.dumps(dict(passed=passed, groups=groups, invalid_cases=invalid, wake_races=wake_races)), flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
