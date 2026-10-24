#!/usr/bin/env python3
"""Guest-only two-context VirGL rendered-pixel and exporter-close regression."""
import argparse
import ctypes
import fcntl
import json
import mmap
import os
import signal
import struct


def ioctl(fd, number, payload):
    value = bytearray(payload)
    fcntl.ioctl(fd, (3 << 30) | (len(value) << 16) | (ord('d') << 8) | number, value, True)
    return value


def clear(fd, handle, resource, color):
    # One context-local surface over the canonical shared texture. Complete
    # cleanup in the stream so later calls can reuse the object name.
    floats = [struct.unpack('<I', struct.pack('<f', x))[0] for x in color]
    words = [(5 << 16) | (8 << 8) | 1, 1, resource, 1, 0, 0,
             (3 << 16) | 5, 1, 0, 1,
             (8 << 16) | 7, 4, *floats, 0, 0, 0,
             (2 << 16) | 5, 0, 0,
             (1 << 16) | (8 << 8) | 3, 1]
    commands = (ctypes.c_uint32 * len(words))(*words)
    handles = (ctypes.c_uint32 * 1)(handle)
    # No requested fence FD: cross-context completion must still be real.
    ioctl(fd, 0x42, struct.pack('<IIQQIiIIIIQQ', 0, ctypes.sizeof(commands),
          ctypes.addressof(commands), ctypes.addressof(handles), 1, -1, 0, 0, 0, 0, 0, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--brook-guest', action='store_true', required=True)
    parser.parse_args()
    signal.alarm(30)
    source = target = prime = -1
    source_handle = imported = 0
    memory = None
    cases = {}
    try:
        source = os.open('/dev/dri/renderD128', os.O_RDWR)
        target = os.open('/dev/dri/renderD128', os.O_RDWR)
        created = struct.unpack('<14I', ioctl(source, 0x44, struct.pack(
            '<14I', 2, 1, 10, 4, 4, 1, 1, 0, 0, 0, 0, 0, 64, 16)))
        source_handle, resource = created[10:12]
        clear(source, source_handle, resource, (1, 0, 0, 1))
        prime = struct.unpack('<IIi', ioctl(source, 0x2d, struct.pack('<IIi', source_handle, 0, -1)))[2]

        def import_handle():
            return struct.unpack('<IIi', ioctl(target, 0x2e, struct.pack('<IIi', 0, 0, prime)))[0]

        imported = import_handle()
        cases['repeated_import_identity'] = import_handle() == imported
        offset = struct.unpack('<QII', ioctl(target, 0x41, struct.pack('<QII', 0, imported, 0)))[0]
        memory = mmap.mmap(target, 4096, flags=mmap.MAP_SHARED,
                           prot=mmap.PROT_READ | mmap.PROT_WRITE, offset=offset, trackfd=False)

        def pixels(expected):
            memory[:64] = bytes(64)
            ioctl(target, 0x46, struct.pack('<11I', imported, 0, 0, 0, 4, 4, 1, 0, 0, 16, 64))
            return memory[:64] == bytes(expected) * 16

        cases['initial_producer_render'] = pixels((0, 0, 255, 255))
        clear(source, source_handle, resource, (0, 1, 0, 1))
        cases['later_producer_render_after_import'] = pixels((0, 255, 0, 255))
        clear(target, imported, resource, (0, 0, 1, 1))
        cases['importer_render'] = pixels((255, 0, 0, 255))
        memory.close();memory = None
        ioctl(target, 0x09, struct.pack('<II', imported, 0));imported = 0
        imported = import_handle()
        offset = struct.unpack('<QII', ioctl(target, 0x41, struct.pack('<QII', 0, imported, 0)))[0]
        memory = mmap.mmap(target, 4096, flags=mmap.MAP_SHARED,
                           prot=mmap.PROT_READ | mmap.PROT_WRITE, offset=offset, trackfd=False)
        clear(target, imported, resource, (1, 0, 1, 1))
        cases['close_and_reimport_render'] = pixels((255, 0, 255, 255))
        ioctl(source, 0x09, struct.pack('<II', source_handle, 0));source_handle = 0
        os.close(source);source = -1
        os.close(prime);prime = -1
        clear(target, imported, resource, (0, 1, 1, 1))
        cases['importer_render_after_exporter_and_prime_close'] = pixels((255, 255, 0, 255))
        print('GPU_PRIME_RENDER_RESULT', json.dumps(dict(passed=all(cases.values()), cases=cases)), flush=True)
        return int(not all(cases.values()))
    finally:
        if memory is not None:
            memory.close()
        if imported and target >= 0:
            ioctl(target, 0x09, struct.pack('<II', imported, 0))
        if source_handle and source >= 0:
            ioctl(source, 0x09, struct.pack('<II', source_handle, 0))
        for fd in (prime, target, source):
            if fd >= 0:
                os.close(fd)
        signal.alarm(0)


if __name__ == '__main__':
    raise SystemExit(main())
