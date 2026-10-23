#!/usr/bin/env python3
"""Run inside a graphics VM: verify DRM render-node SCM_RIGHTS ownership."""
import argparse
import array
import errno
import fcntl
import json
import os
import socket
import struct


def version(fd):
    data = bytearray(64)  # x86-64 struct drm_version; no string buffers requested.
    fcntl.ioctl(fd, 0xC0406400, data, True)
    return struct.unpack_from('iii', data)


def gem(fd):
    payload = bytearray(struct.pack('<14I', 0, 64, 1 << 4, 128, 1, 1, 1, 0, 0, 0, 0, 0, 128, 0))
    fcntl.ioctl(fd, (3 << 30) | (56 << 16) | (ord('d') << 8) | 0x44, payload, True)
    return struct.unpack('<14I', payload)[10]


def close_gem(fd, handle):
    fcntl.ioctl(fd, (3 << 30) | (8 << 16) | (ord('d') << 8) | 0x09,
                struct.pack('<II', handle, 0))


def send(sock, fd):
    assert sock.sendmsg([b'D'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [fd]))]) == 1


def receive(sock):
    data, controls, flags, _ = sock.recvmsg(1, socket.CMSG_SPACE(4), socket.MSG_CMSG_CLOEXEC)
    assert data == b'D' and not flags & socket.MSG_CTRUNC
    rights = []
    for level, kind, raw in controls:
        if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
            rights.extend(array.array('i', raw))
    assert len(rights) == 1
    return rights[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', default='/dev/dri/renderD128')
    args = parser.parse_args()
    def opened():
        return os.open(args.device, os.O_RDWR | os.O_CLOEXEC)
    checks = 0
    for _ in range(16):
        fd = opened()
        expected = version(fd)
        handle = gem(fd)
        a, b = socket.socketpair()
        send(a, fd)
        os.close(fd)  # The queued right is now the sole DRM description owner.
        received = receive(b)
        assert version(received) == expected
        assert fcntl.fcntl(received, fcntl.F_GETFD) & fcntl.FD_CLOEXEC
        duplicate = os.dup(received)
        os.close(received)
        assert version(duplicate) == expected
        close_gem(duplicate, handle)  # The transferred context still owns its GEM table.
        os.close(duplicate)
        a.close(); b.close()
        checks += 1
        # Discard a queued right without receiving it.
        fd = opened(); gem(fd); a, b = socket.socketpair(); send(a, fd); os.close(fd)
        b.close(); a.close(); checks += 1
        # Ancillary truncation must release the undelivered reference.
        fd = opened(); gem(fd); a, b = socket.socketpair(); send(a, fd); os.close(fd)
        data, controls, flags, _ = b.recvmsg(1, 0)
        assert data == b'D' and not controls and flags & socket.MSG_CTRUNC
        b.close(); a.close(); checks += 1
        # A failed send must roll back the acquired message reference.
        fd = opened(); expected = version(fd); a, b = socket.socketpair(); b.close()
        try:
            send(a, fd)
        except OSError as error:
            assert error.errno == errno.EPIPE
        else:
            raise AssertionError('send to closed peer unexpectedly succeeded')
        assert version(fd) == expected
        os.close(fd); a.close(); checks += 1
    fd = opened(); expected = version(fd); a, b = socket.socketpair()
    pid = os.fork()
    if pid == 0:
        try:
            os.close(fd); a.close()
            received = receive(b)
            assert version(received) == expected
            os.close(received); b.close()
        except BaseException:
            os._exit(1)
        os._exit(0)
    b.close(); send(a, fd); os.close(fd); a.close()
    assert os.waitpid(pid, 0) == (pid, 0)
    print(json.dumps(dict(passed=True, cases=checks + 1, version=expected,
                          scope='transfer, last-owner GEM lifetime, dup, fork, discarded/truncated rights and failed-send rollback')),
          flush=True)


if __name__ == '__main__':
    main()
