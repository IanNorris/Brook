#!/usr/bin/env python3
"""Bounded Linux/Brook nested-epoll acceptance; no external dependencies.

Run with Python 3, optionally passing a JSON result path and repeat count
(1–16). Timing assertions
require an otherwise idle guest; results include observed wake latencies.
"""
import ctypes
import errno
import json
import os
import select
import sys
import threading
import time
from contextlib import ExitStack

results = {}

def check(name, fn):
    start = time.monotonic()
    detail = fn()
    results[name] = {"passed": True, "seconds": time.monotonic() - start, "detail": detail}
    print("EPOLL_PASS", name, detail, flush=True)

def expect_errno(number, fn):
    try:
        fn()
    except OSError as e:
        assert e.errno == number, (e.errno, number)
    else:
        raise AssertionError("expected errno %d" % number)

def epolls(stack, count):
    return [stack.enter_context(select.epoll()) for _ in range(count)]

def readiness():
    with ExitStack() as st:
        inner, outer = epolls(st, 2)
        r, w = os.pipe()
        st.callback(os.close, r); st.callback(os.close, w)
        inner.register(r, select.EPOLLIN)
        outer.register(inner, select.EPOLLIN)
        poller = select.poll(); poller.register(outer, select.POLLIN | select.POLLOUT)
        assert poller.poll(0) == []
        assert select.select([outer], [outer], [], 0) == ([], [], [])
        os.write(w, b'x')
        assert outer.poll(0) == [(inner.fileno(), select.EPOLLIN)]
        assert inner.poll(0) == [(r, select.EPOLLIN)]
        assert outer.poll(0) == [(inner.fileno(), select.EPOLLIN)]
        assert poller.poll(0) == [(outer.fileno(), select.POLLIN)]
        # Python exposes only the fd-shaped data field; exercise all 64 bits
        # through the actual packed Linux syscall ABI as well.
        class Event(ctypes.Structure):
            _pack_ = 1
            _fields_ = [('events', ctypes.c_uint32), ('data', ctypes.c_uint64)]
        libc = ctypes.CDLL(None, use_errno=True)
        tagged = Event(select.EPOLLIN, 0xFEEDBEEFDEADBEEF)
        assert libc.epoll_ctl(outer.fileno(), 3, inner.fileno(), ctypes.byref(tagged)) == 0
        observed = Event()
        assert libc.epoll_wait(outer.fileno(), ctypes.byref(observed), 1, 0) == 1
        assert observed.events == select.EPOLLIN and observed.data == tagged.data
        assert inner.poll(0) == [(r, select.EPOLLIN)]
        assert os.read(r, 1) == b'x'
        assert inner.poll(0) == [] and outer.poll(0) == []
        outer.modify(inner, select.EPOLLOUT)
        os.write(w, b'x'); assert outer.poll(0) == []
    return "inner/outer 64-bit data, non-consuming readiness, drain, poll/select, masks"

def controls():
    with ExitStack() as st:
        a, b = epolls(st, 2)
        duplicate = os.dup(a.fileno()); st.callback(os.close, duplicate)
        expect_errno(errno.EINVAL, lambda: a.register(duplicate, select.EPOLLIN))
        a.register(b, select.EPOLLIN)
        expect_errno(errno.EEXIST, lambda: a.register(b, select.EPOLLIN))
        expect_errno(errno.ELOOP, lambda: b.register(a, select.EPOLLIN))
        a.unregister(b)
        expect_errno(errno.ENOENT, lambda: a.modify(b, select.EPOLLIN))
        expect_errno(errno.ENOENT, lambda: a.unregister(b))
        expect_errno(errno.EBADF, lambda: a.register(999999, select.EPOLLIN))
    for reverse in (False, True):
        with ExitStack() as st:
            es = epolls(st, 6)
            for i in (range(4, 0, -1) if reverse else range(4)):
                es[i].register(es[i + 1], select.EPOLLIN)
            edge = 0 if reverse else 4
            expect_errno(errno.ELOOP, lambda: es[edge].register(es[edge + 1], select.EPOLLIN))
    return "self alias, duplicates, cycle, invalid fd, five-node depth both orders"

def blocking_wake():
    latencies = []
    for event in (False, True):
        with ExitStack() as st:
            a, b = epolls(st, 2)
            if event:
                r = w = os.eventfd(0, os.EFD_NONBLOCK); st.callback(os.close, r)
                data = (1).to_bytes(8, 'little')
            else:
                r, w = os.pipe(); st.callback(os.close, r); st.callback(os.close, w)
                data = b'x'
            a.register(r, select.EPOLLIN); b.register(a, select.EPOLLIN)
            def write():
                time.sleep(.02); os.write(w, data)
            t = threading.Thread(target=write); t.start()
            start = time.monotonic(); ready = b.poll(.5); elapsed = time.monotonic() - start
            t.join()
            assert ready == [(a.fileno(), select.EPOLLIN)], ready
            assert elapsed < .08, elapsed  # distinguish the 100 ms safety poll
            latencies.append(elapsed)
    return latencies

def timer():
    class Timespec(ctypes.Structure):
        _fields_ = [('sec', ctypes.c_long), ('nsec', ctypes.c_long)]
    class Itimerspec(ctypes.Structure):
        _fields_ = [('interval', Timespec), ('value', Timespec)]
    libc = ctypes.CDLL(None, use_errno=True)
    fd = libc.timerfd_create(1, 0x800)
    assert fd >= 0, ctypes.get_errno()
    try:
        with ExitStack() as st:
            a, b = epolls(st, 2)
            a.register(fd, select.EPOLLIN); b.register(a, select.EPOLLIN)
            value = Itimerspec(Timespec(0, 0), Timespec(0, 20000000))
            assert libc.timerfd_settime(fd, 0, ctypes.byref(value), None) == 0
            start = time.monotonic(); ready = b.poll(.5); elapsed = time.monotonic() - start
            assert ready == [(a.fileno(), select.EPOLLIN)], ready
            assert elapsed < .08, elapsed
            assert int.from_bytes(os.read(fd, 8), 'little') >= 1
            assert b.poll(0) == []
            return elapsed
    finally:
        os.close(fd)

def diamond():
    with ExitStack() as st:
        root, left, right, child = epolls(st, 4)
        r, w = os.pipe(); st.callback(os.close, r); st.callback(os.close, w)
        child.register(r, select.EPOLLIN)
        left.register(child, select.EPOLLIN); right.register(child, select.EPOLLIN)
        root.register(left, select.EPOLLIN); root.register(right, select.EPOLLIN)
        alias = os.dup(child.fileno()); st.callback(os.close, alias)
        root.register(alias, select.EPOLLIN)
        assert root.poll(0) == []
        os.write(w, b'x')
        assert {fd for fd, ev in root.poll(0)} == {left.fileno(), right.fileno(), alias}
    return "shared child and duplicate aliases"

def mutation():
    for close in (False, True):
        with ExitStack() as st:
            a, b = epolls(st, 2)
            r, w = os.pipe(); st.callback(os.close, w)
            if not close: st.callback(os.close, r)
            a.register(r, select.EPOLLIN); b.register(a, select.EPOLLIN)
            outcome = []
            t = threading.Thread(target=lambda: outcome.append(b.poll(.15)))
            t.start(); time.sleep(.02)
            a.unregister(r)
            if close: os.close(r)
            t.join(2); assert not t.is_alive(), ("nested mutation waiter timed out", close, outcome)
            assert outcome == [[]], outcome
            # Reusing the same underlying pipe must not encounter a stale waiter.
            if not close:
                a.register(r, select.EPOLLIN); os.write(w, b'x')
                assert b.poll(.1) == [(a.fileno(), select.EPOLLIN)]
    return "DEL and close during blocked nested wait"

def closed_watch():
    with ExitStack() as st:
        a, b = epolls(st, 2)
        r, w = os.pipe(); st.callback(os.close, w)
        a.register(r, select.EPOLLIN); b.register(a, select.EPOLLIN)
        os.close(r)
        assert a.poll(0) == []
        assert b.poll(0) == []
        expect_errno(errno.EBADF, lambda: a.unregister(r))
        assert a.poll(0) == []
    return "closed target cannot generate persistent orphan ERR/HUP events"

def killed_waiter():
    for _ in range(8):
        r, w = os.pipe()
        pid = os.fork()
        if pid == 0:
            os.close(r)
            a, b = select.epoll(), select.epoll()
            event = os.eventfd(0, os.EFD_NONBLOCK)
            a.register(event, select.EPOLLIN); b.register(a, select.EPOLLIN)
            os.write(w, b'r')
            b.poll(-1)
            os._exit(99)
        os.close(w)
        try:
            assert os.read(r, 1) == b'r'
            time.sleep(.02)
            os.kill(pid, 9)
            waited, status = os.waitpid(pid, 0)
            assert waited == pid and os.WIFSIGNALED(status) and os.WTERMSIG(status) == 9, status
        finally:
            os.close(r)
    return "eight killed blocked nested waiters reaped"

def concurrent_cycle():
    with ExitStack() as st:
        a, b = epolls(st, 2)
        barrier = threading.Barrier(3); outcomes = []
        def add(x, y):
            barrier.wait()
            try:
                x.register(y, select.EPOLLIN); outcomes.append(0)
            except OSError as e:
                outcomes.append(e.errno)
        threads = [threading.Thread(target=add, args=(a, b)), threading.Thread(target=add, args=(b, a))]
        for t in threads: t.start()
        barrier.wait()
        for t in threads: t.join(2)
        assert all(not t.is_alive() for t in threads)
        assert sorted(outcomes) == [0, errno.ELOOP], outcomes
    return outcomes

try:
    repeats = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    assert 1 <= repeats <= 16
    for iteration in range(repeats):
        for name, fn in [('readiness', readiness), ('controls', controls), ('blocking_wake', blocking_wake),
                         ('timer', timer), ('diamond', diamond), ('mutation', mutation), ('closed_watch', closed_watch),
                         ('concurrent_cycle', concurrent_cycle), ('killed_waiter', killed_waiter)]:
            check(('%d/' % (iteration + 1) if repeats > 1 else '') + name, fn)
except BaseException as e:
    results['error'] = repr(e)
    raise
finally:
    if len(sys.argv) > 1:
        with open(sys.argv[1], 'w') as f:
            json.dump(results, f, indent=2)
    print('EPOLL_RESULT', json.dumps(results), flush=True)
