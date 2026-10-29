#!/usr/bin/env python3
"""Bounded guest TCP close/reuse check; run with the guest's Python 3.

Four workers exercise 64 loopback connections, exact bidirectional payloads,
and listener close while an accepted socket is still owned. No external network,
files, credentials or application state are touched.
"""
import concurrent.futures
import socket


def receive_exact(connection, length):
    data = b""
    while len(data) < length:
        chunk = connection.recv(length - len(data))
        if not chunk:
            raise RuntimeError("unexpected EOF during loopback transfer")
        data += chunk
    return data


def worker(index):
    for sequence in range(16):
        with socket.socket() as listener, socket.socket() as client:
            listener.bind(("127.0.0.1", 0))
            listener.listen(2)
            listener.settimeout(3)
            client.settimeout(3)
            client.connect(listener.getsockname())
            server, _ = listener.accept()
            with server:
                server.settimeout(3)
                payload = bytes([index, sequence]) * 64
                client.sendall(payload)
                assert receive_exact(server, len(payload)) == payload
                server.sendall(payload)
                assert receive_exact(client, len(payload)) == payload
                listener.close()
                client.close()
    return 16


def main():
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        count = sum(executor.map(worker, range(4)))
    assert count == 64
    print("NATIVE_TCP_CLOSE_REUSE_PASS", count, flush=True)


if __name__ == "__main__":
    main()
