#!/usr/bin/env python3
"""Linux/Brook epoll non-pollable-file regression; all waits are bounded."""
import ctypes
import errno
import json
import os
import select
import tempfile


def error(expected, call):
    try:
        call()
    except OSError as exc:
        assert exc.errno == expected, (expected, exc)
    else:
        raise AssertionError('operation unexpectedly succeeded')


def rejected(fd):
    with select.epoll() as ep:
        error(errno.EPERM, lambda: ep.register(fd, select.EPOLLIN | select.EPOLLET))
        error(errno.EPERM, lambda: ep.modify(fd, select.EPOLLIN))
        error(errno.EPERM, lambda: ep.unregister(fd))
        assert ep.poll(0) == []
        event = os.eventfd(1, os.EFD_NONBLOCK)
        try:
            ep.register(event, select.EPOLLIN)
            assert ep.poll(0) == [(event, select.EPOLLIN)]
        finally:
            os.close(event)
    # A library's poll fallback must still observe regular-file readiness.
    poll = select.poll()
    poll.register(fd, select.POLLIN)
    assert poll.poll(0) == [(fd, select.POLLIN)]


def main():
    groups = []
    with tempfile.TemporaryDirectory() as directory:
        with open(directory + '/file', 'w+b') as file:
            rejected(file.fileno())  # EOF remains poll-readable too.
            file.write(b'content'); file.flush(); file.seek(0)
            rejected(file.fileno())
        groups.append('regular-file-and-eof')
        fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
        try:
            rejected(fd)
        finally:
            os.close(fd)
        groups.append('directory')
        fd = os.memfd_create('epoll-file')
        try:
            rejected(fd)
        finally:
            os.close(fd)
        groups.append('memfd')
        with open('/proc/self/status', 'rb') as file:
            rejected(file.fileno())
        groups.append('proc-file')
        # Named FIFOs can share Brook's Vnode type; they must remain watchable.
        path = directory + '/fifo'; os.mkfifo(path)
        reader = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        writer = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
        try:
            with select.epoll() as ep:
                ep.register(reader, select.EPOLLIN)
                assert ep.poll(0) == []
                assert os.write(writer, b'x') == 1
                assert ep.poll(0.5) == [(reader, select.EPOLLIN)]
                assert os.read(reader, 1) == b'x'
                assert ep.poll(0) == []
        finally:
            os.close(writer); os.close(reader)
        groups.append('named-fifo')
    print(json.dumps({'pass': True, 'groups': groups}))


if __name__ == '__main__':
    main()
