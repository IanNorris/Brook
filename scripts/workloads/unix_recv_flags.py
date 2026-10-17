#!/usr/bin/env python3
"""Exercise the Unix recvmsg flags used by Mozilla's audio IPC transport."""
import array
import errno
import fcntl
import os
import select
import socket
import _socket
import traceback


def transfer(flags):
    # Use the C socket objects directly: socket.socketpair() rewraps descriptors
    # and queries getsockname, a separate Brook Unix-address compatibility gap.
    left, right = _socket.socketpair()
    backing = os.memfd_create("brook-audio-ipc-flags", os.MFD_CLOEXEC)
    received = []
    try:
        os.write(backing, b"Brook audio IPC")
        right.setblocking(False)
        with select.epoll() as poll:
            poll.register(right, select.EPOLLIN | select.EPOLLET)
            handles = array.array("i", [backing])
            assert left.sendmsg([b"request"], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, handles)]) == 7
            assert any(fd == right.fileno() and events & select.EPOLLIN
                       for fd, events in poll.poll(1))
            payload, control, result_flags, address = right.recvmsg(
                32, socket.CMSG_SPACE(handles.itemsize), flags)
            for level, kind, data in control:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    values = array.array("i")
                    values.frombytes(data)
                    received.extend(values)
            assert payload == b"request" and not result_flags & socket.MSG_CTRUNC
            assert len(received) == 1
            fd = received[0]
            assert bool(fcntl.fcntl(fd, fcntl.F_GETFD) & fcntl.FD_CLOEXEC) == bool(
                flags & socket.MSG_CMSG_CLOEXEC)
            os.lseek(fd, 0, os.SEEK_SET)
            assert os.read(fd, 32) == b"Brook audio IPC"
            try:
                right.recvmsg(32, 0, flags | socket.MSG_DONTWAIT)
            except BlockingIOError as exc:
                assert exc.errno == errno.EAGAIN
            else:
                raise AssertionError("empty nonblocking receiver did not return EAGAIN")
            left.shutdown(socket.SHUT_WR)
            assert right.recvmsg(32, 0, flags)[0] == b""
    finally:
        for fd in received:
            os.close(fd)
        os.close(backing)
        left.close()
        right.close()


def main():
    cases = [("control", 0), ("nosignal", socket.MSG_NOSIGNAL),
             ("audio-ipc", socket.MSG_NOSIGNAL | socket.MSG_CMSG_CLOEXEC),
             ("audio-ipc-dontwait", socket.MSG_NOSIGNAL | socket.MSG_CMSG_CLOEXEC |
              socket.MSG_DONTWAIT)]
    passed = 0
    for name, flags in cases:
        try:
            transfer(flags)
        except Exception as exc:
            print("UNIX_RECV_FLAGS_CASE", name, "FAIL", repr(exc), flush=True)
            traceback.print_exc()
        else:
            passed += 1
            print("UNIX_RECV_FLAGS_CASE", name, "PASS", flush=True)
    print(f"UNIX_RECV_FLAGS_RESULT {passed}/{len(cases)}", flush=True)
    return 0 if passed == len(cases) else 1


if __name__ == "__main__":
    raise SystemExit(main())
