#!/usr/bin/env python3
"""Real-syscall OSS pointer/geometry/drain regression; run in an isolated VM.

Uses synthetic PCM and libc ioctl directly for interruption tests (Python's
fcntl wrapper may restart EINTR). Does not select an ALSA or browser backend.
"""
import array
import ctypes
import errno
import fcntl
import json
import os
import select
import signal
import threading
import time

RESET, SYNC, TRIGGER = 0x5000, 0x5001, 0x40045010
GETOPTR, GETOSPACE, GETODELAY = 0x800C5012, 0x8010500C, 0x80045017
SETFRAGMENT, SPEED, CHANNELS, FORMAT = 0xC004500A, 0xC0045002, 0xC0045006, 0xC0045005
libc = ctypes.CDLL(None, use_errno=True)
libc.ioctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_void_p]
libc.ioctl.restype = ctypes.c_int


def integer(fd, command, value=0):
    data = array.array('i', [value])
    fcntl.ioctl(fd, command, data, True)
    return data[0]


def query(fd, command, count):
    data = array.array('i', [0] * count)
    fcntl.ioctl(fd, command, data, True)
    return list(data)


def sync(fd):
    result = libc.ioctl(fd, SYNC, None)
    return -ctypes.get_errno() if result < 0 else result


def expect_error(operation, expected):
    try:
        operation()
    except OSError as error:
        assert error.errno == expected, (error, expected)
    else:
        raise AssertionError(('missing error', expected))


def opened():
    return os.open('/dev/dsp', os.O_WRONLY | os.O_NONBLOCK)


def wait_started(fd):
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        if query(fd, GETOPTR, 3)[0]:
            return
        time.sleep(.001)
    raise AssertionError('DMA never started')


def prepare_full(fd):
    fcntl.ioctl(fd, RESET)
    integer(fd, TRIGGER, 0)
    assert os.write(fd, b'\x20\x01' * 32768) == 65536


def main():
    completed = []
    fd, other = opened(), opened()
    duplicate = os.dup(fd)
    try:
        integer(fd, SETFRAGMENT, (4 << 16) | 10)
        assert query(fd, GETOSPACE, 4) == [4, 4, 1024, 4096]
        expect_error(lambda: integer(fd, SETFRAGMENT, (65 << 16) | 10), errno.EINVAL)
        expect_error(lambda: integer(fd, SETFRAGMENT, (4 << 16) | 7), errno.EINVAL)
        assert query(fd, GETOSPACE, 4) == [4, 4, 1024, 4096]
        integer(fd, TRIGGER, 0)
        assert os.write(fd, b'\x20\x01' * 4096) == 4096  # Enforce negotiated limit.
        expect_error(lambda: os.write(fd, b'\x20\x01' * 2), errno.EAGAIN)
        assert query(fd, GETOPTR, 3) == [0, 4, 0]
        assert query(duplicate, GETOPTR, 3) == [0, 4, 0]
        assert libc.ioctl(fd, GETOPTR, ctypes.c_void_p(1)) == -1
        assert ctypes.get_errno() == errno.EFAULT
        assert query(fd, GETOPTR, 3) == [0, 4, 0]
        with select.epoll() as ep:
            ep.register(fd, select.EPOLLOUT)
            assert ep.poll(0) == []
            # Another stream and its physical silence cannot advance this one.
            assert os.write(other, b'\x30\x01' * 8192) == 16384
            assert sync(other) == 0
            assert query(fd, GETOPTR, 3) == [0, 4, 0]
            assert sync(fd) == 0  # Starts a paused finite queue, despite O_NONBLOCK.
            assert ep.poll(.2) == [(fd, select.EPOLLOUT)]
        assert query(fd, GETOPTR, 3) == [4096, 0, 0]
        assert query(duplicate, GETOPTR, 3) == [4096, 0, 0]
        for round_index in range(1, 8):
            assert os.write(fd, b'\x20\x01' * 2048) == 4096
            assert sync(fd) == 0
            assert query(fd, GETOPTR, 3) == [(round_index + 1) * 4096, 0, 0]
        expect_error(lambda: integer(fd, SETFRAGMENT, (2 << 16) | 10), errno.EBUSY)
        fcntl.ioctl(fd, RESET)
        assert query(fd, GETOPTR, 3) == [0, 0, 0]
        assert query(fd, GETOSPACE, 4) == [4, 4, 1024, 4096]
        integer(fd, CHANNELS, 1)
        integer(fd, FORMAT, 8)  # Native-rate U8 mono still has exact frame mapping.
        assert os.write(fd, b'\x80' * 4096) == 4096
        assert sync(fd) == 0 and query(fd, GETOPTR, 3) == [4096, 0, 0]
        completed.append('geometry_pointer_wrap_dup_isolation_epoll_mono')
        print('DSP_ENDPOINT', completed[-1], 'PASS', flush=True)
    finally:
        os.close(duplicate)
        os.close(other)
        os.close(fd)

    fd = opened()
    try:
        integer(fd, SPEED, 48000)
        expect_error(lambda: query(fd, GETOPTR, 3), errno.EOPNOTSUPP)
        integer(fd, SPEED, 44100)
        assert query(fd, GETOPTR, 3) == [0, 0, 0]
        for control in ('reset', 'pause'):
            prepare_full(fd)
            result = []
            worker = threading.Thread(target=lambda: result.append(sync(fd)))
            worker.start()
            wait_started(fd)
            if control == 'reset':
                fcntl.ioctl(fd, RESET)
            else:
                integer(fd, TRIGGER, 0)
            worker.join(3)
            assert not worker.is_alive() and result == [-errno.ECANCELED], (control, result)
            fcntl.ioctl(fd, RESET)
        completed.append('drain_reset_pause_cancel')
        print('DSP_ENDPOINT', completed[-1], 'PASS', flush=True)

        fcntl.ioctl(fd, RESET)
        integer(fd, TRIGGER, 0)
        assert os.write(fd, b'\x20\x01' * 16384) == 32768
        result = []

        def captured_target():
            result.append((sync(fd), integer(fd, GETODELAY)))

        worker = threading.Thread(target=captured_target)
        worker.start()
        wait_started(fd)
        assert os.write(fd, b'\x30\x01' * 16384) == 32768
        worker.join(3)
        assert not worker.is_alive() and result[0][0] == 0 and result[0][1] > 0, result
        assert sync(fd) == 0
        assert query(fd, GETOPTR, 3) == [65536, 0, 0]
        completed.append('drain_captured_target_later_writes')
        print('DSP_ENDPOINT', completed[-1], 'PASS', flush=True)

        observed = []
        previous_handler = signal.signal(signal.SIGUSR1, lambda *_: observed.append('signal'))
        main_thread = threading.get_ident()
        sender = None

        def send_signal():
            time.sleep(.02)
            signal.pthread_kill(main_thread, signal.SIGUSR1)

        try:
            prepare_full(fd)
            signal.siginterrupt(signal.SIGUSR1, True)
            sender = threading.Thread(target=send_signal)
            sender.start()
            assert sync(fd) == -errno.EINTR
            sender.join(3)
            assert not sender.is_alive() and observed == ['signal']
            assert integer(fd, GETODELAY) > 0  # EINTR does not credit the suffix.
            fcntl.ioctl(fd, RESET)
            observed.clear()
            old_mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGUSR1})
            try:
                prepare_full(fd)
                sender = threading.Thread(target=send_signal)
                sender.start()
                assert sync(fd) == 0
                sender.join(3)
                assert not sender.is_alive()
                assert not observed and query(fd, GETOPTR, 3) == [65536, 0, 0]
            finally:
                if sender is not None:
                    sender.join(3)
                signal.pthread_sigmask(signal.SIG_SETMASK, old_mask)
            assert observed == ['signal']
        finally:
            if sender is not None:
                sender.join(3)
            signal.signal(signal.SIGUSR1, previous_handler)
        completed.append('drain_unmasked_masked_signals')
        print('DSP_ENDPOINT', completed[-1], 'PASS', flush=True)
    finally:
        os.close(fd)
    print('DSP_ENDPOINT_PASS', json.dumps(completed), flush=True)


if __name__ == '__main__':
    main()
