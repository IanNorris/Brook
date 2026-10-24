#!/usr/bin/env python3
"""Check Internet socket metadata and Python TLS setup; no remote network required."""
import ctypes
import errno
import json
import os
import socket
import ssl


def main():
    # Linux SOL_SOCKET ABI: SO_PROTOCOL=38, SO_DOMAIN=39.
    checks = 0
    for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
        protocol = socket.IPPROTO_TCP if kind == socket.SOCK_STREAM else socket.IPPROTO_UDP
        for creation_protocol in (0, protocol):
            for flags in (0, socket.SOCK_NONBLOCK | socket.SOCK_CLOEXEC):
                with socket.socket(socket.AF_INET, kind | flags, creation_protocol) as sock:
                    assert sock.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE) == kind
                    assert sock.getsockopt(socket.SOL_SOCKET, 39) == socket.AF_INET
                    assert sock.getsockopt(socket.SOL_SOCKET, 38) == protocol
                    alias = os.dup(sock.fileno())
                    with socket.socket(fileno=alias) as duplicate:
                        assert duplicate.family == socket.AF_INET
                        assert duplicate.type == kind and duplicate.proto == protocol
                        sock.close()
                        assert duplicate.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE) == kind
                        assert duplicate.getsockopt(socket.SOL_SOCKET, 39) == socket.AF_INET
                        assert duplicate.getsockopt(socket.SOL_SOCKET, 38) == protocol
                    checks += 1

    with socket.socket() as listener, socket.socket() as client:
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        client.connect(listener.getsockname())
        accepted, _ = listener.accept()
        with accepted:
            for sock in (listener, client, accepted):
                assert sock.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE) == socket.SOCK_STREAM
                assert sock.getsockopt(socket.SOL_SOCKET, 39) == socket.AF_INET
                assert sock.getsockopt(socket.SOL_SOCKET, 38) == socket.IPPROTO_TCP
                checks += 1

    # Exercise the libc syscall boundary: short/empty output and unsupported
    # options must not overwrite a caller's canary or manufacture success.
    libc = ctypes.CDLL(None, use_errno=True)
    libc.getsockopt.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int,
                               ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint)]
    libc.getsockopt.restype = ctypes.c_int
    with socket.socket() as sock:
        for size in (0, 1, 2, 4, 8):
            output = (ctypes.c_ubyte * 12)(*([0xa5] * 12))
            length = ctypes.c_uint(size)
            assert libc.getsockopt(sock.fileno(), socket.SOL_SOCKET, socket.SO_TYPE,
                                   output, ctypes.byref(length)) == 0
            assert length.value == min(size, 4)
            assert bytes(output[length.value:]) == bytes([0xa5] * (12 - length.value))
            assert bytes(output[:length.value]) == (1).to_bytes(4, 'little')[:length.value]
            checks += 1
        length = ctypes.c_uint(4)
        output = ctypes.c_int(0x12345678)
        assert libc.getsockopt(sock.fileno(), socket.SOL_SOCKET, 0x7fffffff,
                               ctypes.byref(output), ctypes.byref(length)) == -1
        assert ctypes.get_errno() == errno.ENOPROTOOPT
        assert length.value == 4 and output.value == 0x12345678
        checks += 1

    # CPython queries SO_TYPE before creating its SSL object. No handshake or
    # certificate bypass is needed to exercise that setup path on a local fd.
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    with context.wrap_socket(socket.socket(), server_hostname='localhost',
                             do_handshake_on_connect=False) as wrapped:
        assert wrapped.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE) == socket.SOCK_STREAM
        checks += 1
    print(json.dumps(dict(inet_socket_metadata_pass=True, checks=checks,
                          tls_handshake_tested=False)), flush=True)


if __name__ == '__main__':
    main()
