#!/usr/bin/env python3
"""Guest regression for fault-safe TCP read/recvfrom, using a private fixture.

Serve tcp-receive.bin containing PATTERN on the host loopback fixture. Children
receive into inherited CoW mappings; invalid destinations must fail without
consuming the queued prefix. The parent verifies its mapping stayed unchanged.
"""
import argparse
import ctypes as c
import errno
import json
import mmap
import os
import socket
import time

PATTERN = b'Brook TCP receive copy regression\n' * 2048


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--rounds', type=int, choices=range(1, 9), default=2)
    args = parser.parse_args()
    libc = c.CDLL(None, use_errno=True)
    libc.read.argtypes = [c.c_int, c.c_void_p, c.c_size_t]
    libc.read.restype = c.c_ssize_t
    libc.recvfrom.argtypes = [c.c_int, c.c_void_p, c.c_size_t, c.c_int,
                              c.c_void_p, c.c_void_p]
    libc.recvfrom.restype = c.c_ssize_t
    libc.mprotect.argtypes = [c.c_void_p, c.c_size_t, c.c_int]
    libc.mprotect.restype = c.c_int
    results = []
    for repeat in range(args.rounds):
        for api in ('read', 'recvfrom'):
            for case in ('cow', 'invalid', 'readonly', 'cross-page'):
                region = mmap.mmap(-1, 8192, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)
                region[:] = b'Z' * 8192
                address = c.addressof(c.c_char.from_buffer(region))
                client = socket.create_connection(('10.0.2.2', args.port), timeout=10)
                client.settimeout(None)
                client.sendall(b'GET /tcp-receive.bin HTTP/1.0\r\nHost: fixture\r\n\r\n')
                header = bytearray()
                while not header.endswith(b'\r\n\r\n'):
                    value = client.recv(1)
                    assert value and len(header) < 8192
                    header.extend(value)
                assert b' 200 ' in header.split(b'\r\n')[0]
                child = os.fork()
                if not child:
                    try:
                        def receive(pointer, size):
                            c.set_errno(0)
                            if api == 'read':
                                return libc.read(client.fileno(), pointer, size)
                            return libc.recvfrom(client.fileno(), pointer, size, 0, None, None)

                        # TCP's zero-byte receive succeeds without touching memory/data.
                        assert receive(1, 0) == 0
                        target = address
                        count = 64
                        if case == 'invalid':
                            for pointer in (0, 1, 0xffffffffffffffff):
                                assert receive(pointer, count) == -1
                                assert c.get_errno() == errno.EFAULT
                        elif case == 'readonly':
                            assert libc.mprotect(address, 8192, mmap.PROT_READ) == 0
                            assert receive(address, count) == -1 and c.get_errno() == errno.EFAULT
                            assert libc.mprotect(address, 8192, mmap.PROT_READ | mmap.PROT_WRITE) == 0
                        elif case == 'cross-page':
                            target = address + 4096 - 32
                            assert libc.mprotect(address + 4096, 4096, 0) == 0
                            assert receive(target, count) == -1 and c.get_errno() == errno.EFAULT
                            assert libc.mprotect(address + 4096, 4096,
                                                 mmap.PROT_READ | mmap.PROT_WRITE) == 0
                        got = receive(target, count)
                        assert 0 < got <= count
                        assert c.string_at(target, got) == PATTERN[:got]
                        os._exit(0)
                    except BaseException:
                        import traceback
                        traceback.print_exc()
                        os._exit(1)
                deadline = time.monotonic() + 30
                while True:
                    pid, status = os.waitpid(child, os.WNOHANG)
                    if pid:
                        assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, (api, case, status)
                        break
                    assert time.monotonic() < deadline, ('inspect existing child', child)
                    time.sleep(.01)
                assert region[:] == b'Z' * 8192, 'Child receive changed parent CoW mapping'
                client.close()
                region.close()
                results.append(dict(round=repeat, api=api, case=case, passed=True))
    print(json.dumps(dict(passed=True, checks=results)), flush=True)


if __name__ == '__main__':
    main()
