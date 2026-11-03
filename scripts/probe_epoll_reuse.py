#!/usr/bin/env python3
"""Check Linux epoll watch identity across fd reuse and surviving aliases."""
import ctypes
import errno
import os
import select
import sys

class Event(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("events", ctypes.c_uint32), ("data", ctypes.c_uint64)]

libc = ctypes.CDLL(None, use_errno=True)

def ctl(ep, op, fd, tag):
    event = Event(select.EPOLLIN, tag)
    result = libc.epoll_ctl(ep, op, fd, ctypes.byref(event))
    return 0 if result == 0 else ctypes.get_errno()

def ready(ep):
    events = (Event * 4)()
    count = libc.epoll_wait(ep, events, 4, 0)
    if count < 0:
        raise OSError(ctypes.get_errno(), "epoll_wait")
    return {events[i].data for i in range(count)}

def run(keep_alias):
    ep = select.epoll()
    old = os.eventfd(0, os.EFD_NONBLOCK)
    alias = os.dup(old) if keep_alias else -1
    new = -1
    try:
        assert ctl(ep.fileno(), 1, old, 111) == 0
        os.close(old)
        new = os.eventfd(0, os.EFD_NONBLOCK)
        assert new == old, (old, new)
        assert ctl(ep.fileno(), 3, new, 333) == errno.ENOENT, "MOD accepted reused fd"
        assert ctl(ep.fileno(), 1, new, 222) == 0, "ADD rejected reused fd"
        os.write(new, (1).to_bytes(8, "little"))
        if keep_alias:
            os.write(alias, (1).to_bytes(8, "little"))
        expected = {111, 222} if keep_alias else {222}
        assert ready(ep.fileno()) == expected, "wrong tags after fd reuse"
        if keep_alias:
            os.close(alias)
            alias = -1
            assert ready(ep.fileno()) == {222}, "last close retained old watch"
        print("EPOLL_REUSE_PASS", "alias" if keep_alias else "no-alias", flush=True)
    finally:
        if alias >= 0:
            os.close(alias)
        if new >= 0:
            os.close(new)
        ep.close()

def dup2_replacement():
    ep = select.epoll()
    old = os.eventfd(0, os.EFD_NONBLOCK)
    alias = os.dup(old)
    replacement = os.eventfd(0, os.EFD_NONBLOCK)
    try:
        assert ctl(ep.fileno(), 1, old, 111) == 0
        os.dup2(replacement, old)
        assert ctl(ep.fileno(), 3, old, 333) == errno.ENOENT
        assert ctl(ep.fileno(), 1, old, 222) == 0
        os.write(alias, (1).to_bytes(8, "little"))
        os.write(replacement, (1).to_bytes(8, "little"))
        assert ready(ep.fileno()) == {111, 222}, "dup2 lost or confused a watch"
        os.close(alias)
        alias = -1
        assert ready(ep.fileno()) == {222}, "dup2 retained watch after final close"
        print("EPOLL_REUSE_PASS dup2", flush=True)
    finally:
        if alias >= 0:
            os.close(alias)
        os.close(old)
        os.close(replacement)
        ep.close()

def inherited():
    ep = select.epoll()
    watched = os.eventfd(0, os.EFD_NONBLOCK)
    read_end, write_end = os.pipe()
    release_read, release_write = os.pipe()
    try:
        assert ctl(ep.fileno(), 1, watched, 111) == 0
        pid = os.fork()
        if pid == 0:
            os.close(read_end)
            os.close(release_write)
            try:
                os.close(watched)
                replacement = os.eventfd(0, os.EFD_NONBLOCK)
                assert replacement == watched
                assert ctl(ep.fileno(), 1, replacement, 222) == 0
                os.write(replacement, (1).to_bytes(8, "little"))
                os.write(write_end, b"1")
                # Keep this description alive until the parent inspects both.
                assert os.read(release_read, 1) == b"1"
                os.close(replacement)
                os._exit(0)
            except BaseException:
                os._exit(1)
        os.close(write_end)
        write_end = -1
        os.close(release_read)
        release_read = -1
        assert os.read(read_end, 1) == b"1", "child failed before ready"
        os.write(watched, (1).to_bytes(8, "little"))
        assert ready(ep.fileno()) == {111, 222}, "fork confused descriptor tables"
        os.write(release_write, b"1")
        _, status = os.waitpid(pid, 0)
        assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0
        assert ready(ep.fileno()) == {111}, "child close retained its watch"
        print("EPOLL_REUSE_PASS inherited", flush=True)
    finally:
        os.close(read_end)
        if write_end >= 0:
            os.close(write_end)
        if release_read >= 0:
            os.close(release_read)
        os.close(release_write)
        os.close(watched)
        ep.close()

cases = {"no-alias": lambda: run(False), "alias": lambda: run(True),
         "dup2": dup2_replacement, "inherited": inherited}
requested = sys.argv[1:] or list(cases)
for name in requested:
    cases[name]()
