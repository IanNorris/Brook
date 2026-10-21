#!/usr/bin/env python3
"""Bounded full-pipe write probe; a delayed reader releases broken kernels.

Reports behavior instead of hanging or leaving an orphan. Test both F_SETFL
and pipe2. Filling capacity-1 accommodates Brook's one-slot ring; Linux leaves
one byte free, insufficient for the atomic two-byte probe there as well.
"""
import errno
import argparse
import fcntl
import json
import os
import threading
import time


def probe(mode, capacity_override=None):
    r, w = os.pipe2(os.O_NONBLOCK) if mode == 'pipe2' else os.pipe()
    try:
        if mode == 'fcntl':
            fcntl.fcntl(w, fcntl.F_SETFL, fcntl.fcntl(w, fcntl.F_GETFL) | os.O_NONBLOCK)
        flags = fcntl.fcntl(w, fcntl.F_GETFL)
        assert flags & os.O_NONBLOCK
        capacity_source = 'F_GETPIPE_SZ'
        try:
            capacity = fcntl.fcntl(w, fcntl.F_GETPIPE_SZ)
        except OSError as e:
            if e.errno not in (errno.EINVAL, errno.ENOTTY, errno.ENOSYS) or capacity_override is None:
                raise
            capacity = capacity_override
            capacity_source = 'explicit_kernel_configuration'
        assert 2 <= capacity <= 1048576
        filled = 0
        while filled < capacity - 1:
            n = os.write(w, b'x' * min(4096, capacity - 1 - filled))
            assert n > 0
            filled += n
        drain = {}

        def delayed_reader():
            time.sleep(.1)
            drain['before_ns'] = time.monotonic_ns()
            drain['bytes'] = len(os.read(r, 2))
            drain['after_ns'] = time.monotonic_ns()

        worker = threading.Thread(target=delayed_reader)
        worker.start()
        begin = time.monotonic_ns()
        returned = error = None
        try:
            returned = os.write(w, b'yz')
        except OSError as e:
            error = e.errno
        end = time.monotonic_ns()
        worker.join()
        assert drain['bytes'] == 2
        return dict(mode=mode, flags=flags, capacity=capacity, capacity_source=capacity_source, filled=filled,
                    returned=returned, error=error, begin_ns=begin, end_ns=end,
                    elapsed_ns=end-begin, delayed_reader=drain,
                    nonblocking_correct=error == errno.EAGAIN and returned is None,
                    returned_before_reader=end <= drain['before_ns'])
    finally:
        os.close(w)
        os.close(r)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capacity', type=int, help='Known configured capacity for kernels without F_GETPIPE_SZ')
    args = parser.parse_args()
    rows = [probe(mode, args.capacity) for mode in ('fcntl', 'pipe2')]
    print(json.dumps(dict(cases=rows, passed=all(v['nonblocking_correct'] for v in rows))))
