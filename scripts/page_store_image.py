#!/usr/bin/env python3
"""Create/inspect disposable Brook raw page backing. Never formats an existing file."""
import argparse
import json
from pathlib import Path
import struct
import uuid

MAGIC = b"BRKSWP01"
HEADER = struct.Struct("<8sIIQQ16sQ")

def checksum(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value

def inspect(path):
    with path.open("rb") as stream:
        block = stream.read(4096)
    if len(block) != 4096:
        raise ValueError("short header")
    magic, version, page, slots, offset, identity, crc = HEADER.unpack_from(block)
    if (magic != MAGIC or version != 1 or page != 4096 or offset != 4096
            or not 0 < slots < 0xffffffff or not any(identity)
            or crc != checksum(block[:48]) or any(block[HEADER.size:])
            or offset + slots * page > path.stat().st_size):
        raise ValueError("invalid page-store header/capacity")
    return dict(version=version, page_bytes=page, slots=slots,
                identity=str(uuid.UUID(bytes=identity)), capacity_bytes=slots * page,
                scope="Disposable backing only; slot ownership/checksums are runtime state, not crash recovery")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("create", "inspect"))
    parser.add_argument("image", type=Path)
    parser.add_argument("--mib", type=int, default=256)
    args = parser.parse_args()
    if args.action == "create":
        slots = args.mib * 256
        if not 0 < slots < 0xffffffff:
            parser.error("capacity must be 1..16777215 MiB")
        prefix = struct.pack("<8sIIQQ16s", MAGIC, 1, 4096, slots, 4096, uuid.uuid4().bytes)
        with args.image.open("xb") as stream:
            stream.write(prefix + struct.pack("<Q", checksum(prefix)))
            stream.truncate(4096 + slots * 4096)
    print(json.dumps(inspect(args.image), indent=2))

if __name__ == "__main__":
    main()
