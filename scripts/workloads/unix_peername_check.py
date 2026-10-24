#!/usr/bin/env python3
"""Linux/Brook connected Unix stream address and lifetime conformance control."""
import ctypes as c
import errno
import json
import os
from pathlib import Path
import socket
import tempfile


def main():
    cases = {}

    def peer(label, endpoint, expected):
        try:
            cases[label] = endpoint.getpeername() == expected
        except OSError:
            cases[label] = False

    left, right = socket.socketpair()
    try:
        peer('pair_left_unnamed', left, '')
        peer('pair_right_unnamed', right, '')
        right.close()
        peer('pair_name_after_peer_close', left, '')
    finally:
        left.close()
        right.close()

    with tempfile.TemporaryDirectory(prefix='brook-peer-') as directory:
        server_path = str(Path(directory) / 'server')
        client_path = str(Path(directory) / 'client')
        listener = socket.socket(socket.AF_UNIX)
        client = socket.socket(socket.AF_UNIX)
        unnamed = socket.socket(socket.AF_UNIX)
        accepted = other = None
        try:
            listener.bind(server_path)
            listener.listen(4)
            client.bind(client_path)
            client.connect(server_path)
            # The client's peer name must already be available before accept.
            peer('client_before_accept', client, server_path)
            accepted, _ = listener.accept()
            peer('client_named_peer', client, server_path)
            peer('accepted_named_peer', accepted, client_path)
            cases['client_local_name_preserved'] = client.getsockname() == client_path
            cases['accepted_local_name_preserved'] = accepted.getsockname() == server_path
            unnamed.connect(server_path)
            other, _ = listener.accept()
            peer('accepted_unnamed_peer', other, '')

            libc = c.CDLL(None, use_errno=True)
            get = libc.getpeername
            get.argtypes = [c.c_int, c.c_void_p, c.c_void_p]
            get.restype = c.c_int
            expected = b'\x01\x00' + os.fsencode(server_path) + b'\x00'
            for capacity in (0, 1, 2, 7, 110):
                buffer = (c.c_ubyte * 112)(*([0xa5] * 112))
                size = c.c_uint(capacity)
                result = get(client.fileno(), buffer if capacity else None, c.byref(size))
                copied = min(capacity, len(expected))
                cases['raw_capacity_' + str(capacity)] = (
                    result == 0 and size.value == len(expected) and
                    bytes(buffer[:copied]) == expected[:copied] and
                    all(v == 0xa5 for v in buffer[copied:]))

            size = c.c_uint(8)
            c.set_errno(0)
            cases['bad_address_efault'] = get(client.fileno(), c.c_void_p(1), c.byref(size)) == -1 and c.get_errno() == errno.EFAULT
            c.set_errno(0)
            cases['bad_length_efault'] = get(client.fileno(), c.c_void_p(1), c.c_void_p(1)) == -1 and c.get_errno() == errno.EFAULT
            size = c.c_uint(0xffffffff)
            c.set_errno(0)
            cases['negative_length_einval'] = get(client.fileno(), c.c_void_p(1), c.byref(size)) == -1 and c.get_errno() == errno.EINVAL

            os.unlink(server_path)
            os.unlink(client_path)
            listener.close()
            peer('client_name_after_unlink_and_listener_close', client, server_path)
            peer('accepted_name_after_unlink', accepted, client_path)
            client.close()
            peer('accepted_name_after_client_close', accepted, client_path)
        finally:
            for endpoint in (other, accepted, unnamed, client, listener):
                if endpoint is not None:
                    endpoint.close()

    unconnected = socket.socket(socket.AF_UNIX)
    try:
        try:
            unconnected.getpeername()
            cases['unconnected_enotconn'] = False
        except OSError as error:
            cases['unconnected_enotconn'] = error.errno == errno.ENOTCONN
    finally:
        unconnected.close()
    print('UNIX_PEERNAME_RESULT', json.dumps(dict(passed=all(cases.values()), cases=cases)), flush=True)
    return int(not all(cases.values()))


if __name__ == '__main__':
    raise SystemExit(main())
