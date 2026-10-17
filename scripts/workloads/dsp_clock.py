#!/usr/bin/env python3
"""Verify per-stream software+hardware queue accounting inside Brook.

Run in an isolated VM with silent host output. This is not ALSA/Firefox or a
complete OSS endpoint acceptance test: GETOPTR, drain and epoll are separate.
"""
import array
import errno
import fcntl
import json
import os
import select
import time


def integer(fd, command, value=0):
    argument = array.array('i', [value])
    fcntl.ioctl(fd, command, argument, True)
    return argument[0]


def delay(fd):
    return integer(fd, 0x80045017)


def space(fd):
    argument = array.array('i', [0, 0, 0, 0])
    fcntl.ioctl(fd, 0x8010500C, argument, True)
    return argument[3]


def pause(fd):
    integer(fd, 0x40045010, 0)


def start(fd):
    integer(fd, 0x40045010, 2)


def wait_empty(fd):
    began = time.monotonic()
    observations = []
    while time.monotonic() - began < 5:
        remaining = delay(fd)
        observations.append(remaining)
        assert 0 <= remaining <= 65536, remaining
        if not remaining:
            return {'elapsed_s': time.monotonic() - began,
                    'samples': observations}
        time.sleep(0.005)
    raise AssertionError(('queue did not drain', observations))


def main():
    handles = []
    try:
        for _ in range(2):
            fd = os.open('/dev/dsp', os.O_WRONLY | os.O_NONBLOCK)
            handles.append(fd)
            pause(fd)
        a, b = handles
        tone = array.array('h', [value for i in range(2048)
                               for value in ([1500, 1500] if i % 100 < 50 else [-1500, -1500])]).tobytes()
        assert os.write(a, tone[:4096]) == 4096
        assert os.write(b, tone) == 8192
        assert delay(a) == 4096 and delay(b) == 8192
        assert space(a) == 65536 - 4096 and space(b) == 65536 - 8192
        try:
            integer(a, 0xC0045002, 48000)
        except OSError as error:
            assert error.errno == errno.EBUSY, error
        else:
            raise AssertionError('format changed while old audio was outstanding')
        start(b)
        b_result = wait_empty(b)
        assert delay(a) == 4096, 'another stream advanced the paused stream'
        os.close(handles.pop())
        start(a)
        a_result = wait_empty(a)  # 4096 bytes is below legacy start threshold.
        assert space(a) == 65536
        print('DSP_CLOCK independent_completion PASS', json.dumps({'a_short_tail': a_result, 'b': b_result}), flush=True)
        pause(a)
        full = (tone * 8)
        assert len(full) == 65536 and os.write(a, full) == 65536
        poller = select.poll()
        poller.register(a, select.POLLOUT)
        assert not poller.poll(0), 'full paused stream reported writable'
        fcntl.ioctl(a, 0x5000)
        assert poller.poll(0) == [(a, select.POLLOUT)]
        assert delay(a) == 0 and space(a) == 65536
        print('DSP_CLOCK poll_backpressure_reset PASS', flush=True)
        print('DSP_CLOCK_PASS', flush=True)
    finally:
        for fd in handles:
            os.close(fd)


if __name__ == '__main__':
    main()
