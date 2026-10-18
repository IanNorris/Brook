#!/usr/bin/env python3
"""Linux/Brook eventfd epoll regressions; all waits are bounded.

These cover eventfd notifications, not ET support for other descriptor types.
Run unchanged on Linux as the reference and inside an isolated Brook guest.
"""
import ctypes
import errno
import json
import os
import select
import struct
import signal
import threading
import time
from contextlib import contextmanager


@contextmanager
def event(count=1, semaphore=False, nonblock=True):
    fd = os.eventfd(count, (os.EFD_NONBLOCK if nonblock else 0) |
                    (os.EFD_SEMAPHORE if semaphore else 0))
    try:
        yield fd
    finally:
        os.close(fd)


def expect(poll, fd=None, mask=select.EPOLLIN):
    result = poll.poll(0, 8)
    assert result == ([] if fd is None else [(fd, mask)]), result


def undrained():
    with event() as fd, select.epoll() as poll:
        poll.register(fd, select.EPOLLIN | select.EPOLLET)
        expect(poll, fd)
        expect(poll)
        # A write is a new notification even while already readable.
        os.eventfd_write(fd, 2)
        expect(poll, fd)
        expect(poll)
        os.eventfd_write(fd, 0)
        expect(poll, fd)
        expect(poll)
        assert os.eventfd_read(fd) == 3
        expect(poll)
        os.eventfd_write(fd, 0)
        expect(poll)
        # Drain and refill between waits defeats sampled-readiness emulation.
        os.eventfd_write(fd, 1)
        expect(poll, fd)
        assert os.eventfd_read(fd) == 1
        os.eventfd_write(fd, 1)
        expect(poll, fd)
        expect(poll)


def semaphore():
    with event(3, True) as fd, select.epoll() as poll:
        poll.register(fd, select.EPOLLIN | select.EPOLLET)
        expect(poll, fd)
        assert os.eventfd_read(fd) == 1
        expect(poll)  # Read generated OUT, not another subscribed IN.
        os.eventfd_write(fd, 0)
        expect(poll, fd)
        expect(poll)
        # When eligible, return the full requested current readiness mask.
        poll.modify(fd, select.EPOLLIN | select.EPOLLOUT | select.EPOLLET)
        expect(poll, fd, select.EPOLLIN | select.EPOLLOUT)
        expect(poll)
        assert os.eventfd_read(fd) == 1
        expect(poll, fd, select.EPOLLIN | select.EPOLLOUT)
        expect(poll)


def writable():
    with event() as fd, select.epoll() as poll:
        poll.register(fd, select.EPOLLOUT | select.EPOLLET)
        expect(poll, fd, select.EPOLLOUT)
        expect(poll)
        assert os.eventfd_read(fd) == 1
        expect(poll, fd, select.EPOLLOUT)
        expect(poll)
        os.eventfd_write(fd, 1)
        expect(poll)  # IN-only notification must not queue an OUT-only watch.


def rearm_and_independent():
    with event() as fd, select.epoll() as first, select.epoll() as second:
        alias = os.dup(fd)
        try:
            first.register(fd, select.EPOLLIN | select.EPOLLET)
            second.register(alias, select.EPOLLIN | select.EPOLLET)
            expect(first, fd)
            expect(first)
            expect(second, alias)
            expect(second)
            first.modify(fd, select.EPOLLIN | select.EPOLLET)
            expect(first, fd)
            expect(first)
            expect(second)
            os.eventfd_write(alias, 0)
            expect(first, fd)
            expect(second, alias)
        finally:
            os.close(alias)


def truncated_batch():
    with event() as first, event() as second, select.epoll() as poll:
        for fd in (first, second):
            poll.register(fd, select.EPOLLIN | select.EPOLLET)
        a = poll.poll(0, 1)
        b = poll.poll(0, 1)
        assert len(a) == len(b) == 1 and {a[0][0], b[0][0]} == {first, second}, (a, b)
        expect(poll)


def nested_peek():
    with event() as fd, select.epoll() as inner, select.epoll() as outer:
        inner.register(fd, select.EPOLLIN | select.EPOLLET)
        outer.register(inner.fileno(), select.EPOLLIN)
        peek = select.poll()
        peek.register(inner.fileno(), select.POLLIN)
        for _ in range(3):
            assert peek.poll(0) == [(inner.fileno(), select.POLLIN)]
            expect(outer, inner.fileno())
        expect(inner, fd)
        expect(inner)
        expect(outer)
        assert peek.poll(0) == []


def failed_copy():
    # Valid mapped-but-readonly destination: no consumed edge on EFAULT.
    libc = ctypes.CDLL(None, use_errno=True)
    libc.mmap.restype = ctypes.c_void_p
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                          ctypes.c_int, ctypes.c_int, ctypes.c_long]
    libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    libc.epoll_wait.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    address = libc.mmap(None, 4096, 1, 0x22, -1, 0)
    assert address not in (None, ctypes.c_void_p(-1).value)
    try:
        with event() as fd, select.epoll() as poll:
            poll.register(fd, select.EPOLLIN | select.EPOLLET)
            ctypes.set_errno(0)
            assert libc.epoll_wait(poll.fileno(), address, 1, 0) == -1
            assert ctypes.get_errno() == errno.EFAULT, ctypes.get_errno()
            expect(poll, fd)
            expect(poll)
    finally:
        assert libc.munmap(address, 4096) == 0


def saturation_and_level():
    with event(0) as fd, select.epoll() as poll:
        os.eventfd_write(fd, (1 << 64) - 2)
        poll.register(fd, select.EPOLLIN | select.EPOLLOUT)
        expect(poll, fd)
        expect(poll, fd)  # LT stays ready without a new write.
        try:
            os.eventfd_write(fd, 1)
        except BlockingIOError as error:
            assert error.errno == errno.EAGAIN
        else:
            raise AssertionError('eventfd counter overflow accepted')
        try:
            os.write(fd, struct.pack('Q', (1 << 64) - 1))
        except OSError as error:
            assert error.errno == errno.EINVAL
        else:
            raise AssertionError('reserved eventfd value accepted')
        assert os.eventfd_read(fd) == (1 << 64) - 2
        expect(poll, fd, select.EPOLLOUT)


def concurrent_drain():
    with event(0) as fd, select.epoll() as poll:
        poll.register(fd, select.EPOLLIN | select.EPOLLET)
        errors = []
        def produce():
            try:
                for _ in range(1000):
                    os.eventfd_write(fd, 1)
            except Exception as error:
                errors.append(repr(error))
        workers = [threading.Thread(target=produce, daemon=True) for _ in range(4)]
        for worker in workers:
            worker.start()
        total = 0
        deadline = time.monotonic() + 10
        while total < 4000 and time.monotonic() < deadline:
            for ready_fd, mask in poll.poll(0.05, 1):
                assert ready_fd == fd and mask == select.EPOLLIN
                while True:
                    try:
                        total += os.eventfd_read(fd)
                    except BlockingIOError:
                        break
        for worker in workers:
            worker.join(2)
        assert not any(worker.is_alive() for worker in workers)
        assert not errors and total == 4000, (errors, total)
        # A queued notification can refer to data already drained; it must
        # not make a currently empty source readable.
        expect(poll)


def blocked_writer():
    fd = os.eventfd(0, 0)
    writer = None
    done = threading.Event()
    errors = []
    def write():
        try:
            os.eventfd_write(fd, 1)
        except Exception as error:
            errors.append(repr(error))
        finally:
            done.set()
    try:
        os.eventfd_write(fd, (1 << 64) - 2)
        writer = threading.Thread(target=write, daemon=True)
        writer.start()
        assert not done.wait(0.03), errors
        assert os.eventfd_read(fd) == (1 << 64) - 2
        assert done.wait(2), 'blocked writer made no progress after read'
        writer.join(2)
        assert not errors, errors
        assert os.eventfd_read(fd) == 1
    finally:
        if writer is None or not writer.is_alive():
            os.close(fd)


def killed_writer():
    with event(0, nonblock=False) as fd:
        os.eventfd_write(fd, (1 << 64) - 2)
        child = os.fork()
        if child == 0:
            try:
                os.eventfd_write(fd, 1)
            finally:
                os._exit(3)
        try:
            time.sleep(0.03)
            assert os.waitpid(child, os.WNOHANG) == (0, 0)
            os.kill(child, signal.SIGKILL)
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                pid, status = os.waitpid(child, os.WNOHANG)
                if pid:
                    child = 0
                    assert os.WIFSIGNALED(status) and os.WTERMSIG(status) == signal.SIGKILL
                    break
                time.sleep(0.01)
            assert child == 0, 'killed blocked writer was not reaped'
            assert os.eventfd_read(fd) == (1 << 64) - 2
            os.eventfd_write(fd, 1)
            assert os.eventfd_read(fd) == 1
        finally:
            if child > 0:
                try:
                    os.kill(child, signal.SIGKILL)
                except ProcessLookupError:
                    pass


def main():
    report = {}
    for check in (undrained, semaphore, writable, rearm_and_independent,
                  truncated_batch, nested_peek, failed_copy, saturation_and_level,
                  concurrent_drain, blocked_writer, killed_writer):
        try:
            check()
            report[check.__name__] = {'pass': True}
        except Exception as error:
            report[check.__name__] = {'pass': False, 'error': repr(error)}
    print(json.dumps(report, sort_keys=True), flush=True)
    if not all(item['pass'] for item in report.values()):
        raise SystemExit(1)
    print('EVENTFD_EDGES_PASS', flush=True)


if __name__ == '__main__':
    main()
