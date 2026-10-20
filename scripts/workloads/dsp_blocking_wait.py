#!/usr/bin/env python3
"""Verify a paused/full blocking DSP writer waits for reset, not a time limit.

Use in an isolated guest. Queued PCM is silence and remains paused; cleanup
resets and closes the private description. All waits and thread joins are bound.
"""
import array
import errno
import fcntl
import json
import os
import threading
import time


def integer(fd, request, value):
    data = array.array('i', [value])
    fcntl.ioctl(fd, request, data, True)
    return data[0]


def main():
    fd = os.open('/dev/dsp', os.O_WRONLY | os.O_NONBLOCK)
    started, finished = threading.Event(), threading.Event()
    result = {}
    writer = None
    try:
        for request, value in ((0xC0045005, 16), (0xC0045006, 2), (0xC0045002, 44100)):
            assert integer(fd, request, value) == value
        integer(fd, 0x40045010, 0)  # Pause only this stream.
        assert os.write(fd, bytes(65536)) == 65536
        space = array.array('i', [0] * 4)
        fcntl.ioctl(fd, 0x8010500C, space, True)
        assert space[3] == 0, list(space)
        try:
            os.write(fd, bytes(4))
        except OSError as error:
            assert error.errno == errno.EAGAIN, error
            result['nonblocking_errno'] = error.errno
        else:
            raise AssertionError('full nonblocking stream accepted more data')
        fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) & ~os.O_NONBLOCK)
        result['fd_flags'] = fcntl.fcntl(fd, fcntl.F_GETFL)

        def write():
            result['started_ns'] = time.monotonic_ns()
            started.set()
            try:
                result['accepted'] = os.write(fd, bytes(4))
            except OSError as error:
                result['errno'] = error.errno
            finally:
                result['finished_ns'] = time.monotonic_ns()
                finished.set()

        writer = threading.Thread(target=write, daemon=True)
        writer.start()
        assert started.wait(2), 'writer did not start'
        time.sleep(1.2)
        result['returned_before_reset'] = finished.is_set()
        result['released_ns'] = time.monotonic_ns()
        fcntl.ioctl(fd, 0x5000)  # Reset from another thread must release capacity.
        writer.join(5)
        assert not writer.is_alive(), 'writer did not finish after reset'
        result['passed'] = (not result['returned_before_reset'] and
                            result.get('accepted') == 4 and 'errno' not in result)
    finally:
        fcntl.ioctl(fd, 0x5000)
        if writer is not None:
            writer.join(5)
        os.close(fd)
    print(json.dumps(result), flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
