#!/usr/bin/env python3
"""Bounded TCP sink for tcp_send_chunks.c; records host-side receipt timing."""
import argparse
import hashlib
import json
import socket
import time
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--bind", required=True, help="Explicit local IPv4 address")
parser.add_argument("--port", required=True, type=int)
parser.add_argument("--connections", required=True, type=int)
parser.add_argument("--output", required=True, type=Path)
parser.add_argument("--bytes", type=int, default=4 * 1024 * 1024, help="Payload length (1 through 64 MiB)")
args = parser.parse_args()
if not 1 <= args.port <= 65535 or not 1 <= args.connections <= 32:
    parser.error("port must be 1..65535 and connections 1..32")
if not 1 <= args.bytes <= 64 * 1024 * 1024:
    parser.error("bytes must be 1..67108864")
TOTAL = args.bytes
pattern = bytes((i * 17 + 3) & 255 for i in range(256))
expected = hashlib.sha256()
for _ in range(TOTAL // 256):
    expected.update(pattern)
expected.update(pattern[:TOTAL % 256])
EXPECTED = expected.hexdigest()

records = []
with args.output.open("x", encoding="utf-8") as result:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.settimeout(60)
        listener.bind((args.bind, args.port))
        listener.listen(1)
        print("TCP_CHUNK_SINK_READY", flush=True)
        for _ in range(args.connections):
            with listener.accept()[0] as connection:
                connection.settimeout(60)
                connection.sendall(b"B")
                start = time.monotonic_ns()
                digest = hashlib.sha256()
                received = 0
                while received < TOTAL:
                    data = connection.recv(min(65536, TOTAL - received))
                    if not data:
                        raise RuntimeError(f"short transfer: {received} bytes")
                    digest.update(data)
                    received += len(data)
                elapsed = time.monotonic_ns() - start
                # The marker follows exactly TOTAL data bytes. It avoids
                # requiring TCP half-close support from the guest.
                if connection.recv(1) != b"E":
                    raise RuntimeError("missing or misplaced end marker")
                checksum = digest.hexdigest()
                if checksum != EXPECTED:
                    raise RuntimeError(f"digest mismatch: {checksum}")
                connection.sendall(b"K")
                records.append({"bytes": received, "sha256": checksum, "elapsed_ns": elapsed})
                result.seek(0)
                json.dump(records, result, indent=2)
                result.write("\n")
                result.truncate()
                result.flush()
                print("TCP_CHUNK_SINK_COMPLETE", len(records), elapsed, flush=True)
