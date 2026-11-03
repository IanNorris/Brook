#!/usr/bin/env python3
"""Check Linux epoll watch identity across fd reuse and surviving aliases."""
import ctypes
import errno
import os
import select

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

run(False)
run(True)
