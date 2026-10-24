#!/usr/bin/env python3
"""INET nonblocking controls, aliases and receive/accept behavior on loopback."""
import ctypes
import errno
import fcntl
import json
import os
import select
import socket
import struct
import time


def flag(fd):
    return bool(fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_NONBLOCK)


def mode(fd, enabled, ioctl):
    if ioctl:
        fcntl.ioctl(fd, 0x5421, struct.pack('i', int(enabled)))
    else:
        old = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL,
                    old | os.O_NONBLOCK if enabled else old & ~os.O_NONBLOCK)


def readable(sock):
    poller = select.poll()
    poller.register(sock, select.POLLIN)
    assert poller.poll(3000)


def delayed_send(peer, data=b'x'):
    pid = os.fork()
    if not pid:
        try:
            time.sleep(.1)
            peer.sendall(data)
        except BaseException:
            os._exit(1)
        os._exit(0)
    return pid


def reaped(pid):
    assert os.waitpid(pid, 0)[1] == 0


def connection(nonblock=False):
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(8)
        client = socket.socket(socket.AF_INET, socket.SOCK_STREAM |
                               (socket.SOCK_NONBLOCK if nonblock else 0))
        result = client.connect_ex(listener.getsockname())
        assert result == (errno.EINPROGRESS if nonblock else 0), result
        poller = select.poll()
        poller.register(client, select.POLLOUT)
        assert poller.poll(3000)
        assert client.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR) == 0
        peer, _ = listener.accept()
        return client, peer


def main():
    checks = 0
    for ioctl in (False, True):
        for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
            with socket.socket(socket.AF_INET, kind) as original:
                alias = os.dup(original.fileno())
                try:
                    for fd in (original.fileno(), alias):
                        mode(fd, True, ioctl)
                        assert flag(original) and flag(alias)
                        mode(fd, False, ioctl)
                        assert not flag(original) and not flag(alias)
                        checks += 1
                finally:
                    os.close(alias)
    # Creation and both post-creation controls must select the same async path.
    for method in range(3):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM |
                               (socket.SOCK_NONBLOCK if method == 0 else 0)) as client:
                if method:
                    mode(client, True, method == 2)
                assert flag(client)
                assert client.connect_ex(listener.getsockname()) == errno.EINPROGRESS
                peer, _ = listener.accept()
                peer.close()
                checks += 1
    # Clearing through an alias must override a stale creation-time FD bit.
    for ioctl in (False, True):
        for operation in ('read', 'recv', 'recvmsg'):
            client, peer = connection(True)
            with client, peer:
                alias = os.dup(client.fileno())
                try:
                    mode(alias, False, ioctl)
                    assert not flag(client) and not flag(alias)
                    child = delayed_send(peer)
                    try:
                        if operation == 'read':
                            value = os.read(client.fileno(), 1)
                        elif operation == 'recv':
                            value = client.recv(1)
                        else:
                            value = client.recvmsg(1)[0]
                        assert value == b'x'
                    finally:
                        reaped(child)
                    checks += 1
                finally:
                    os.close(alias)
    # Per-call MSG_DONTWAIT must not change shared status.
    client, peer = connection()
    with client, peer:
        alias = os.dup(client.fileno())
        try:
            for operation in ('recv', 'recvmsg'):
                child = delayed_send(peer)
                try:
                    try:
                        if operation == 'recv':
                            client.recv(1, socket.MSG_DONTWAIT)
                        else:
                            client.recvmsg(1, 0, socket.MSG_DONTWAIT)
                    except BlockingIOError as error:
                        assert error.errno == errno.EAGAIN
                    else:
                        raise AssertionError('MSG_DONTWAIT blocked or consumed delayed data')
                    assert not flag(client) and not flag(alias)
                    readable(client)
                    assert client.recv(1) == b'x'
                finally:
                    reaped(child)
                checks += 1
        finally:
            os.close(alias)
    # Two nonblocking readers race for one byte. A delayed second byte bounds a
    # broken blocking path and distinguishes it from the expected EAGAIN.
    client, peer = connection(True)
    with client, peer:
        peer.sendall(b'z')
        readable(client)
        gate_read, gate_write = os.pipe()
        children = []
        for _ in range(2):
            pid = os.fork()
            if not pid:
                try:
                    os.read(gate_read, 1)
                    value = client.recv(1)
                    os._exit(10 if value == b'z' else 12)
                except BlockingIOError:
                    os._exit(11)
                except BaseException:
                    os._exit(13)
            children.append(pid)
        sender = delayed_send(peer, b'g')
        os.write(gate_write, b'!!')
        outcomes = [os.waitpid(pid, 0)[1] for pid in children]
        reaped(sender)
        os.close(gate_read)
        os.close(gate_write)
        assert sorted(outcomes) == [10 << 8, 11 << 8], outcomes
        checks += 1
    # Nonblocking listener does not make an ordinary accepted fd nonblocking.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM | socket.SOCK_NONBLOCK) as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        try:
            listener.accept()
        except BlockingIOError:
            pass
        else:
            raise AssertionError('empty nonblocking accept succeeded')
        with socket.socket() as client:
            client.connect(listener.getsockname())
            peer, _ = listener.accept()
            with peer:
                assert not flag(peer)
                checks += 1
    # A creation-time nonblocking listener can become blocking through an alias.
    for ioctl in (False, True):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM | socket.SOCK_NONBLOCK) as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            alias = os.dup(listener.fileno())
            mode(alias, False, ioctl)
            assert not flag(listener) and not flag(alias)
            address = listener.getsockname()
            child = os.fork()
            if not child:
                try:
                    time.sleep(.1)
                    with socket.socket() as client:
                        client.connect(address)
                except BaseException:
                    os._exit(1)
                os._exit(0)
            try:
                peer, _ = listener.accept()
                with peer:
                    assert not flag(peer)
                    checks += 1
            finally:
                os.close(alias)
                reaped(child)
    # accept4's flags apply to the child, not the listener's waiting policy.
    libc = ctypes.CDLL(None, use_errno=True)
    libc.accept4.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    libc.accept4.restype = ctypes.c_int
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        address = listener.getsockname()
        child = os.fork()
        if not child:
            try:
                time.sleep(.1)
                with socket.socket() as client:
                    client.connect(address)
            except BaseException:
                os._exit(1)
            os._exit(0)
        accepted = libc.accept4(listener.fileno(), None, None, socket.SOCK_NONBLOCK)
        try:
            assert accepted >= 0, ctypes.get_errno()
            assert flag(accepted) and not flag(listener)
            checks += 1
        finally:
            if accepted >= 0:
                os.close(accepted)
            reaped(child)
    print(json.dumps(dict(inet_nonblocking_pass=True, checks=checks,
                          send_ack_wait_tested=False, udp_blocking_receive_tested=False)), flush=True)


if __name__ == '__main__':
    main()
