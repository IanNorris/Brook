#!/usr/bin/env python3
"""Linux/guest SCM_RIGHTS lifetime regressions for Wine-style IPC handles."""
import array
import ctypes
import errno
import json
import os
import select
import socket
import struct


def main():
    cases = []

    def check(name, condition):
        cases.append({'name': name, 'pass': bool(condition)})
        assert condition, name

    def send(a, fd):
        assert a.sendmsg([b'x'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                 array.array('i', [fd]))]) == 1

    def receive(b):
        data, ancillary, flags, _ = b.recvmsg(1, socket.CMSG_SPACE(4), socket.MSG_CMSG_CLOEXEC)
        assert data == b'x' and not flags & socket.MSG_CTRUNC
        rights = array.array('i')
        for level, kind, payload in ancillary:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                rights.frombytes(payload)
        assert len(rights) == 1
        fd = rights[0]
        check('received descriptor is close-on-exec', not os.get_inheritable(fd))
        return fd

    try:
        # A queued reference owns the endpoint even before recvmsg installs it.
        for end in (0, 1):
            a, b = socket.socketpair()
            read, write = os.pipe()
            try:
                send(a, (read, write)[end])
                os.close((read, write)[end])
                fd = receive(b)
                try:
                    os.write(write if end == 0 else fd, b'pipe')
                    check('pipe endpoint survives sender close ' + str(end),
                          os.read(fd if end == 0 else read, 4) == b'pipe')
                finally:
                    os.close(fd)
                if end == 1:
                    check('received writer close produces EOF', os.read(read, 1) == b'')
            finally:
                os.close(write if end == 0 else read)
                a.close(); b.close()

        for discard in ('socket-close', 'control-truncation', 'invalid-group'):
            a, b = socket.socketpair()
            read, write = os.pipe2(os.O_NONBLOCK)
            try:
                if discard == 'invalid-group':
                    try:
                        a.sendmsg([b'x'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                           array.array('i', [write, -1]))])
                        raise AssertionError('invalid descriptor accepted')
                    except OSError as error:
                        check('invalid group rejected atomically', error.errno == errno.EBADF)
                else:
                    send(a, write)
                os.close(write)
                if discard == 'socket-close':
                    b.close()
                elif discard == 'control-truncation':
                    data, ancillary, flags, _ = b.recvmsg(1, 0)
                    check('truncation reported', data == b'x' and not ancillary and flags & socket.MSG_CTRUNC)
                else:
                    b.setblocking(False)
                    try:
                        b.recv(1)
                        raise AssertionError('rejected payload was queued')
                    except BlockingIOError:
                        check('invalid group queues no payload', True)
                check('discarded rights release writer ' + discard, os.read(read, 1) == b'')
            finally:
                os.close(read); a.close(); b.close()

        a, b = socket.socketpair()
        event = os.eventfd(3, os.EFD_NONBLOCK)
        try:
            send(a, event); os.close(event)
            fd = receive(b)
            try:
                check('eventfd counter survives sender close', os.read(fd, 8) == struct.pack('Q', 3))
                os.write(fd, struct.pack('Q', 7))
                check('received eventfd remains writable', os.read(fd, 8) == struct.pack('Q', 7))
            finally:
                os.close(fd)
        finally:
            a.close(); b.close()

        a, b = socket.socketpair()
        epoll = select.epoll()
        read, write = os.pipe()
        try:
            epoll.register(read, select.EPOLLIN)
            send(a, epoll.fileno()); epoll.close()
            received = select.epoll.fromfd(receive(b))
            try:
                os.write(write, b'e')
                check('epoll interest survives sender close', (read, select.EPOLLIN) in received.poll(1))
            finally:
                received.close()
        finally:
            epoll.close(); os.close(read); os.close(write); a.close(); b.close()

        libc = ctypes.CDLL(None, use_errno=True)
        timer = libc.timerfd_create(1, os.O_NONBLOCK | os.O_CLOEXEC)
        if timer < 0:
            raise OSError(ctypes.get_errno(), 'timerfd_create')
        a, b = socket.socketpair()
        try:
            send(a, timer); os.close(timer)
            fd = receive(b)
            try:
                # Two timespecs: no repeat, initial expiry one nanosecond.
                value = (ctypes.c_long * 4)(0, 0, 0, 1)
                assert libc.timerfd_settime(fd, 0, ctypes.byref(value), None) == 0
                ready, _, _ = select.select([fd], [], [], 2)
                check('timerfd expires after sender close', fd in ready and struct.unpack('Q', os.read(fd, 8))[0] >= 1)
            finally:
                os.close(fd)
        finally:
            a.close(); b.close()

        for path in ('/dev/null', '/dev/urandom'):
            a, b = socket.socketpair()
            original = os.open(path, os.O_RDWR)
            try:
                send(a, original); os.close(original)
                fd = receive(b)
                try:
                    check('stateless device survives transfer ' + path,
                          os.write(fd, b'x') == 1 if path == '/dev/null' else len(os.read(fd, 8)) == 8)
                finally:
                    os.close(fd)
            finally:
                a.close(); b.close()
    except Exception as error:
        print(json.dumps({'pass_all': False, 'cases': cases, 'error': repr(error)}))
        return 1
    print(json.dumps({'pass_all': True, 'cases': cases}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
