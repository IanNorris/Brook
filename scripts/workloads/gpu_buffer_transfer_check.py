#!/usr/bin/env python3
"""Guest-only virtio DRM byte-buffer upload/readback regression.

Run on isolated Brook GL disks. Checks full upload first, then a partial upload
whose byte offset must not be scaled as a four-byte texture pixel coordinate.
"""
import fcntl
import json
import mmap
import os
import struct
import sys


def ioctl(fd, number, payload):
    data = bytearray(payload)
    fcntl.ioctl(fd, (3 << 30) | (len(data) << 16) | (ord('d') << 8) | number, data, True)
    return data


def main():
    if sys.argv[1:] != ['--brook-guest']:
        raise SystemExit('Usage in an isolated Brook GL VM: gpu_buffer_transfer_check.py --brook-guest')
    fd = os.open('/dev/dri/renderD128', os.O_RDWR)
    memory = None
    handle = 0
    try:
        # PIPE_BUFFER, R8_UNORM, vertex bind; resource creation creates context.
        create = struct.unpack('<14I', ioctl(fd, 0x44, struct.pack(
            '<14I', 0, 64, 1 << 4, 128, 1, 1, 1, 0, 0, 0, 0, 0, 128, 0)))
        handle, resource = create[10:12]
        offset, _, _ = struct.unpack('<QII', ioctl(fd, 0x41, struct.pack('<QII', 0, handle, 0)))
        memory = mmap.mmap(fd, 4096, flags=mmap.MAP_SHARED,
                           prot=mmap.PROT_READ | mmap.PROT_WRITE, offset=offset)

        def transfer(number, x, width, backing_offset):
            ioctl(fd, number, struct.pack('<11I', handle, x, 0, 0, width, 1, 1,
                                          0, backing_offset, 0, 0))

        original = bytes(range(128))
        memory[:128] = original
        transfer(0x47, 0, 128, 0)
        memory[:128] = bytes(128)
        transfer(0x46, 0, 128, 0)
        assert memory[:128] == original, 'Full-buffer upload/readback failed; partial case is not isolated'
        print('GPU_BUFFER_FULL_TRANSFER_PASS', json.dumps({'handle': handle, 'resource': resource}), flush=True)
        expected = bytearray(original)
        expected[16:32] = bytes([0xa5]) * 16
        memory[16:32] = expected[16:32]
        transfer(0x47, 16, 16, 16)
        memory[:128] = bytes(128)
        transfer(0x46, 0, 128, 0)
        actual = memory[:128]
        mismatch = [i for i in range(128) if actual[i] != expected[i]]
        print('GPU_BUFFER_PARTIAL_TRANSFER', json.dumps({
            'pass': not mismatch, 'mismatch_offsets': mismatch,
            'expected_slice': list(expected[16:32]), 'actual_slice': list(actual[16:32]),
            'unchanged_prefix': actual[:16] == original[:16],
            'unchanged_suffix': actual[32:] == original[32:]}), flush=True)
        if mismatch:
            return 1
        # Resource x and guest-backing offset need not match, in either
        # direction. Check that the destination's other bytes stay untouched.
        memory[:128] = bytes([0xee]) * 128
        transfer(0x46, 48, 16, 80)
        read_expected = bytearray([0xee] * 128)
        read_expected[80:96] = original[48:64]
        assert memory[:128] == read_expected, 'Partial readback ignored resource x or backing offset'
        print('GPU_BUFFER_PARTIAL_READBACK_PASS', flush=True)
        memory[96:112] = bytes([0xb6]) * 16
        transfer(0x47, 64, 16, 96)
        expected[64:80] = bytes([0xb6]) * 16
        memory[:128] = bytes(128)
        transfer(0x46, 0, 128, 0)
        assert memory[:128] == expected, 'Upload ignored independent backing offset'
        print('GPU_BUFFER_INDEPENDENT_OFFSET_PASS', flush=True)
        return 0
    finally:
        if memory is not None:
            memory.close()
        if handle:
            ioctl(fd, 0x09, struct.pack('<II', handle, 0))
        os.close(fd)


if __name__ == '__main__':
    raise SystemExit(main())
