#!/usr/bin/env python3
"""Own-socket inode/procfs conformance; exports counts only, never other sockets."""
import array
import concurrent.futures
import errno
import json
import os
import socket
import stat
import sys


def identity(fd):
    info = os.fstat(fd)
    assert stat.S_ISSOCK(info.st_mode) and info.st_ino
    path = '/proc/self/fd/' + str(fd)
    assert os.readlink(path) == 'socket:[' + str(info.st_ino) + ']'
    assert stat.S_ISLNK(os.lstat(path).st_mode)
    follow = os.stat(path)
    assert (follow.st_dev, follow.st_ino, follow.st_uid, follow.st_gid) == (
        info.st_dev, info.st_ino, info.st_uid, info.st_gid)
    assert stat.S_ISSOCK(follow.st_mode)
    return info.st_ino


def table(sock, inode, name='tcp'):
    rows = [line.split() for line in open('/proc/net/' + name)]
    # Scope to our inode, never emit the system-wide table.
    matches = [r for r in rows[1:] if len(r) >= 10 and r[9] == str(inode)]
    assert len(matches) == 1
    row = matches[0]
    assert int(row[1].split(':')[1], 16) == sock.getsockname()[1]
    assert int(row[7]) == os.fstat(sock.fileno()).st_uid


def main():
    if '--owner-change' in sys.argv:
        assert os.geteuid() == 0
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0)); listener.listen()
            pid = os.fork()
            if not pid:
                try:
                    parent = os.getppid()
                    os.setegid(1000); os.seteuid(1000)
                    try:
                        os.stat('/proc/' + str(parent) + '/fd/' + str(listener.fileno()))
                    except OSError as error:
                        assert error.errno in (errno.EACCES, errno.EPERM)
                    else:
                        raise AssertionError('cross-owner socket metadata allowed')
                    with socket.socket() as client:
                        client.connect(listener.getsockname())
                        accepted, _ = listener.accept()
                        with accepted:
                            assert os.fstat(listener.fileno()).st_uid == 0
                            info = os.fstat(accepted.fileno())
                            assert (info.st_uid, info.st_gid) == (1000, 1000)
                            table(accepted, identity(accepted.fileno()))
                except BaseException:
                    os._exit(1)
                os._exit(0)
            _, status = os.waitpid(pid, 0)
            assert status == 0
        print(json.dumps(dict(acceptor_ownership_pass=True)), flush=True)
        return
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0)); listener.listen()
        original = identity(listener.fileno()); table(listener, original)
        parent = os.getpid(); pid = os.fork()
        if not pid:
            try:
                info = os.stat('/proc/' + str(parent) + '/fd/' + str(listener.fileno()))
                assert info.st_ino == original and stat.S_ISSOCK(info.st_mode)
            except BaseException:
                os._exit(1)
            os._exit(0)
        _, status = os.waitpid(pid, 0); assert status == 0
        alias = os.dup(listener.fileno())
        try:
            assert identity(alias) == original
            with socket.socket() as client:
                client.connect(listener.getsockname())
                accepted, _ = listener.accept()
                with accepted:
                    peer = identity(accepted.fileno()); caller = identity(client.fileno())
                    assert len({original, peer, caller}) == 3
                    table(accepted, peer); table(client, caller)
        finally:
            os.close(alias)
    # O_PATH pins immutable metadata across transport close and FD/slot reuse.
    sock = socket.socket()
    old_fd = sock.fileno(); old = identity(old_fd)
    path = '/proc/self/fd/' + str(old_fd)
    pin = os.open(path, os.O_PATH)
    try:
        try:
            unexpected = os.open(path, os.O_RDONLY)
        except OSError as error:
            assert error.errno == errno.ENXIO
        else:
            os.close(unexpected); raise AssertionError('ordinary socket path reopened')
        sock.close()
        with socket.socket() as replacement:
            assert identity(replacement.fileno()) != old
            assert os.fstat(pin).st_ino == old and stat.S_ISSOCK(os.fstat(pin).st_mode)
    finally:
        sock.close(); os.close(pin)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.bind(('127.0.0.1', 0)); table(udp, identity(udp.fileno()), 'udp')

    left, right = socket.socketpair()
    passed = socket.socket()
    received = None
    try:
        original = identity(passed.fileno())
        left.sendmsg([b'x'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                              array.array('i', [passed.fileno()]))])
        data, ancillary, _, _ = right.recvmsg(1, socket.CMSG_SPACE(4))
        assert data == b'x' and len(ancillary) == 1
        assert ancillary[0][:2] == (socket.SOL_SOCKET, socket.SCM_RIGHTS)
        descriptors = array.array('i'); descriptors.frombytes(ancillary[0][2])
        assert len(descriptors) == 1; received = descriptors[0]
        passed.close()
        assert identity(received) == original
    finally:
        if received is not None:
            os.close(received)
        passed.close(); left.close(); right.close()

    def churn(_):
        seen = set()
        for _ in range(100):
            with socket.socket() as sock:
                ino = identity(sock.fileno())
                assert ino not in seen; seen.add(ino)
                alias = os.dup(sock.fileno())
                try:
                    sock.close()
                    assert identity(alias) == ino
                finally:
                    os.close(alias)
        return seen
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(churn, range(8)))
    assert len(set.union(*results)) == sum(map(len, results)) == 800
    print(json.dumps(dict(socket_identity_pass=True, concurrent_lifetimes=800,
                         tcp_endpoints=3, udp=True, path_retention=True,
                         scm_rights_identity=True)), flush=True)


if __name__ == '__main__':
    main()
