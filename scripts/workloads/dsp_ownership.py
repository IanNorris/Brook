#!/usr/bin/env python3
"""Exercise Brook's per-open OSS queue lifecycle inside an isolated VM.

This verifies software queue ownership, not hardware completion or audio clocks.
The VM must use a silent host audio backend.
"""
import array
import errno
import fcntl
import json
import os
import threading
import time

RESET = 0x5000
SETTRIGGER = 0x40045010
GETOSPACE = 0x8010500C


def trigger(fd, enabled):
    fcntl.ioctl(fd, SETTRIGGER, array.array('i', [2 if enabled else 0]))


def space(fd):
    value = array.array('i', [0, 0, 0, 0])
    fcntl.ioctl(fd, GETOSPACE, value, True)
    return value[3]


def expect_full():
    try:
        fd = os.open('/dev/dsp', os.O_WRONLY | os.O_NONBLOCK)
    except OSError as error:
        assert error.errno == errno.EBUSY, error
    else:
        os.close(fd)
        raise AssertionError('ninth stream unexpectedly opened')


def main():
    handles = []
    try:
        for _ in range(8):
            handles.append(os.open('/dev/dsp', os.O_WRONLY | os.O_NONBLOCK))
        expect_full()
        alias = os.dup(handles[0])
        os.close(handles[0])
        handles[0] = alias
        expect_full()  # dup does not allocate a stream or release the shared one
        for fd in handles:
            trigger(fd, False)
        capacity = space(handles[0])
        assert capacity == 65536, capacity
        data = b'\x10\x00\x10\x00' * (capacity // 4)
        assert os.write(handles[0], data) == capacity
        assert space(handles[0]) == 0
        try:
            os.write(handles[0], data[:4])
        except BlockingIOError as error:
            assert error.errno == errno.EAGAIN, error
        else:
            raise AssertionError('full nonblocking queue accepted a frame')
        assert os.write(handles[1], data[:4096]) == 4096
        remaining = space(handles[1])
        fcntl.ioctl(handles[0], RESET)
        assert space(handles[0]) == capacity
        assert space(handles[1]) == remaining
        os.close(handles[0])
        handles[0] = os.open('/dev/dsp', os.O_WRONLY | os.O_NONBLOCK)
        trigger(handles[0], False)
        assert space(handles[0]) == capacity
        assert space(handles[1]) == remaining
        print('DSP_OWNERSHIP independent_reset_close_dup PASS', flush=True)
        for requested, expected in [(1, 4000), (2147483647, 192000), (44100, 44100)]:
            rate = array.array('i', [requested])
            fcntl.ioctl(handles[0], 0xC0045002, rate, True)
            assert rate[0] == expected, list(rate)
            assert 0 < space(handles[0]) <= 2147483647
        print('DSP_OWNERSHIP rate_bounds PASS', flush=True)
        fragment = array.array('i', [(4 << 16) | 16])
        try:
            fcntl.ioctl(handles[0], 0xC004500A, fragment, True)
        except OSError as error:
            assert error.errno == errno.EINVAL, error
        else:
            raise AssertionError('oversized explicit geometry silently accepted')
        fragment[0] = (1 << 16) | 16
        fcntl.ioctl(handles[0], 0xC004500A, fragment, True)
        fragment[0] = 0
        fcntl.ioctl(handles[0], 0xC0045004, fragment, True)
        assert fragment[0] == 65536, list(fragment)
        print('DSP_OWNERSHIP fragment_64k PASS', flush=True)



        # A writer must release its per-open mutex while waiting, so another
        # thread can reset the paused full queue and allow it to proceed.
        fd = handles[0]
        assert os.write(fd, data) == capacity
        fcntl.fcntl(fd, fcntl.F_SETFL, 0)
        result = []
        entered = threading.Event()
        def writer():
            entered.set()
            try:
                result.append(os.write(fd, data[:16]))
            except BaseException as error:
                result.append(repr(error))
        thread = threading.Thread(target=writer, daemon=True)
        thread.start()
        assert entered.wait(2)
        time.sleep(0.05)
        assert thread.is_alive(), result
        fcntl.ioctl(fd, RESET)
        thread.join(3)
        assert not thread.is_alive(), 'reset blocked behind waiting writer'
        assert result == [16], result
        assert space(fd) == capacity - 16
        print('DSP_OWNERSHIP blocked_writer_reset PASS', flush=True)

        # A forked description keeps its stream until the last reference exits.
        ready_r, ready_w = os.pipe()
        release_r, release_w = os.pipe()
        pid = os.fork()
        if pid == 0:
            try:
                os.close(ready_r); os.close(release_w)
                os.write(ready_w, b'R')
                assert os.read(release_r, 1) == b'X'
            except BaseException:
                os._exit(1)
            os._exit(0)
        os.close(ready_w); os.close(release_r)
        assert os.read(ready_r, 1) == b'R'
        os.close(handles.pop())
        expect_full()
        os.write(release_w, b'X')
        _, status = os.waitpid(pid, 0)
        assert status == 0, status
        for pipe in (ready_r, release_w):
            os.close(pipe)
        handles.append(os.open('/dev/dsp', os.O_WRONLY | os.O_NONBLOCK))
        print('DSP_OWNERSHIP fork_final_close PASS', flush=True)
        print('DSP_OWNERSHIP_PASS', json.dumps({'capacity_bytes': capacity,
              'checks': ['independent_reset_close_dup', 'rate_bounds', 'fragment_64k', 'blocked_writer_reset', 'fork_final_close']}), flush=True)
    finally:
        for fd in handles:
            os.close(fd)


if __name__ == '__main__':
    main()
