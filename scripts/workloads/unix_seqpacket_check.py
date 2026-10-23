#!/usr/bin/env python3
"""Compare AF_UNIX sequenced-record IPC against Linux using synthetic data."""
import array
import errno
import fcntl
import json
import os
import select
import signal
import socket
import struct
import tempfile
from pathlib import Path

checks = 0

def check(value):
    global checks
    checks += 1
    assert value, checks


def pair():
    return socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)


def no_message(peer):
    try:
        peer.recvmsg(64, 256, socket.MSG_DONTWAIT)
    except OSError as error:
        check(error.errno == errno.EAGAIN)
    else:
        check(False)


def rights(ancillary):
    result = []
    for level, kind, data in ancillary:
        if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
            values = array.array('i')
            values.frombytes(data[:len(data) // values.itemsize * values.itemsize])
            result.extend(values)
    return result


def main():
    signal.alarm(60)
    with tempfile.TemporaryDirectory(prefix='brook-seqpacket-') as directory:
        # Truncation drops only the remainder of one record.
        a, b = pair()
        try:
            a.send(b'first'); a.send(b'second-record')
            data, anc, flags, _ = b.recvmsg(2)
            check(data == b'fi' and bool(flags & socket.MSG_TRUNC) and not anc)
            check(b.recv(64) == b'second-record'); no_message(b)
            a.sendmsg([b'AB', b'CD'])
            x, y = bytearray(1), bytearray(3)
            n, anc, flags, _ = b.recvmsg_into([x, y])
            check(n == 4 and x + y == b'ABCD' and not flags and not anc)
            check(os.writev(a.fileno(), [b'EF', b'GH']) == 4)
            x, y = bytearray(2), bytearray(6)
            check(os.readv(b.fileno(), [x, y]) == 4 and x + y[:2] == b'EFGH')
            a.send(b'12345'); a.send(b'tail')
            target = bytearray(2)
            n, _, flags, _ = b.recvmsg_into([target], 0, socket.MSG_TRUNC)
            check(n == 5 and target == b'12' and bool(flags & socket.MSG_TRUNC))
            check(b.recv(64) == b'tail')
            a.send(b'keep'); check(os.read(b.fileno(), 0) == b''); check(b.recv(64) == b'keep')
            for send in [lambda: a.send(b''), lambda: os.write(a.fileno(), b''), lambda: a.sendmsg([])]:
                check(send() == 0)
                poller = select.poll(); poller.register(b, select.POLLIN)
                check(any(mask & select.POLLIN for _, mask in poller.poll(0)))
                check(b.recvmsg(1)[0] == b''); no_message(b)
            a.send(b'abc'); a.send(b'defghi')
            available = array.array('i', [0]); fcntl.ioctl(b, 0x541B, available, True)
            check(available[0] == 9)
            check(b.recv(64) == b'abc' and b.recv(64) == b'defghi')
            try: a.send(b'x' * (1024 * 1024), socket.MSG_DONTWAIT)
            except OSError as error: check(error.errno == errno.EMSGSIZE)
            else: check(False)
            no_message(b)
        finally: a.close(); b.close()

        # Empty records and adjacent rights groups share byte offsets, but must
        # keep distinct descriptor ownership and close-on-exec state.
        a, b = pair(); r1, w1 = os.pipe(); r2, w2 = os.pipe()
        try:
            os.write(w1, b'1'); os.write(w2, b'2')
            check(a.sendmsg([], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [r1]))]) == 0)
            check(a.sendmsg([b'next'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [r2]))]) == 4)
            os.close(r1); r1 = -1; os.close(r2); r2 = -1
            for expected, payload in [(b'1', b''), (b'2', b'next')]:
                data, anc, flags, _ = b.recvmsg(64, 256, socket.MSG_CMSG_CLOEXEC)
                received = rights(anc)
                try:
                    check(data == payload and not (flags & ~socket.MSG_CMSG_CLOEXEC) and len(received) == 1)
                    check(os.read(received[0], 1) == expected)
                    check(bool(fcntl.fcntl(received[0], fcntl.F_GETFD) & fcntl.FD_CLOEXEC))
                finally:
                    for fd in received: os.close(fd)
            no_message(b)
        finally:
            a.close(); b.close()
            for fd in [r1, w1, r2, w2]:
                if fd >= 0: os.close(fd)

        # Truncated ancillary data and plain read must release undisclosed
        # capabilities. With every reader gone, the pipe writer observes EPIPE.
        for plain_read in [False, True]:
            a, b = pair(); r, w = os.pipe(); delivered = []
            previous = signal.signal(signal.SIGPIPE, signal.SIG_IGN)
            try:
                a.sendmsg([b'xyz'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [r, r]))])
                os.close(r); r = -1
                if plain_read:
                    check(os.read(b.fileno(), 1) == b'x')
                else:
                    data, anc, flags, _ = b.recvmsg(1, socket.CMSG_LEN(4))
                    delivered = rights(anc)
                    check(data == b'x' and bool(flags & socket.MSG_CTRUNC) and bool(flags & socket.MSG_TRUNC))
                    check(len(delivered) == 1)
                    os.close(delivered.pop())
                try: os.write(w, b'z')
                except OSError as error: check(error.errno == errno.EPIPE)
                else: check(False)
                no_message(b)
            finally:
                for fd in delivered: os.close(fd)
                if r >= 0: os.close(r)
                os.close(w); a.close(); b.close(); signal.signal(signal.SIGPIPE, previous)

        # Queue-full failure publishes no short or additional record.
        a, b = pair()
        try:
            a.setblocking(False); b.setblocking(False); sent = 0; block = b'q' * 20000
            for _ in range(4096):
                try: check(a.send(block) == len(block)); sent += 1
                except OSError as error:
                    check(error.errno == errno.EAGAIN); break
            else: check(False)
            check(sent > 0)
            for _ in range(sent): check(b.recv(30000) == block)
            no_message(b)
            a.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1280); empty = 0
            for _ in range(4096):
                try: check(a.send(b'') == 0); empty += 1
                except OSError as error:
                    check(error.errno == errno.EAGAIN); break
            else: check(False)
            for _ in range(empty): check(b.recvmsg(1)[0] == b'')
            no_message(b)
            a.shutdown(socket.SHUT_WR); check(b.recvmsg(1)[0] == b'')
        finally: a.close(); b.close()

        # Chromium's PID oracle passes an endpoint, then obtains the sending
        # child's credentials through that inherited endpoint.
        a, b = pair(); c, d = socket.socketpair()
        try:
            b.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
            c.sendmsg([b'F'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [a.fileno()]))])
            _, anc, _, _ = d.recvmsg(1, 256)
            transferred = rights(anc); check(len(transferred) == 1)
            forwarded = socket.socket(fileno=transferred[0]); a.close()
            child = os.fork()
            if child == 0:
                try: forwarded.send(b'PING'); os._exit(0)
                except BaseException: os._exit(1)
            data, anc, flags, _ = b.recvmsg(64, 256)
            check(data == b'PING' and not flags)
            credentials = [struct.unpack('3i', value) for level, kind, value in anc
                           if level == socket.SOL_SOCKET and kind == socket.SCM_CREDENTIALS]
            check(credentials == [(child, os.getuid(), os.getgid())])
            check(os.waitpid(child, 0)[1] == 0); forwarded.close()
        finally: a.close(); b.close(); c.close(); d.close()

        # Listening/accepted peers preserve the transport type.
        path = str(Path(directory) / 'listener')
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        accepted = None
        try:
            listener.bind(path); listener.listen(1); client.connect(path)
            accepted, _ = listener.accept()
            check(accepted.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE) == socket.SOCK_SEQPACKET)
            client.send(b'one'); client.send(b'two')
            check(accepted.recv(64) == b'one' and accepted.recv(64) == b'two')
        finally:
            if accepted is not None: accepted.close()
            client.close(); listener.close(); os.unlink(path)
    signal.alarm(0)
    print(json.dumps(dict(unix_seqpacket_pass=True, checks=checks,
                          coverage='boundaries/truncation/vectors/empty/rights/backpressure/credentials/accept')), flush=True)


if __name__ == '__main__':
    main()
