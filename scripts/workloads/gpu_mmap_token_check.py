#!/usr/bin/env python3
"""Guest DRM stale-token and retained-mapping regression on private GL disks.

Creates three tiny buffers. Checks GEM-slot and DRM-file reuse, old mappings
surviving handle/FD close, and shared device mappings across fork, including
a child retaining them after the parent unmaps. This does not establish safe
host-resource retirement or shared PRIME backing.
"""
import argparse
import errno
import fcntl
import json
import mmap
import os
import struct


def ioctl(fd, number, payload):
    data = bytearray(payload)
    fcntl.ioctl(fd, (3 << 30) | (len(data) << 16) | (ord('d') << 8) | number, data, True)
    return data


def create(fd):
    values = struct.unpack('<14I', ioctl(fd, 0x44, struct.pack(
        '<14I', 0, 64, 1 << 4, 128, 1, 1, 1, 0, 0, 0, 0, 0, 128, 0)))
    return values[10]


def token(fd, handle):
    return struct.unpack('<QII', ioctl(fd, 0x41, struct.pack('<QII', 0, handle, 0)))[0]


def mapped(fd, offset):
    # CPython otherwise duplicates fd inside mmap, preventing the explicit
    # os.close(fd) below from exercising the DRM context's final FD close.
    return mmap.mmap(fd, 4096, flags=mmap.MAP_SHARED,
                     prot=mmap.PROT_READ | mmap.PROT_WRITE, offset=offset,
                     trackfd=False)


def rejected(fd, offset):
    try:
        memory = mapped(fd, offset)
    except OSError as error:
        if error.errno != errno.EINVAL:
            raise
        return True
    else:
        memory.close()
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--brook-guest', required=True, action='store_true')
    parser.parse_args()
    fd = os.open('/dev/dri/renderD128', os.O_RDWR)
    reopened = -1
    memory_a = memory_b = None
    try:
        handle_a = create(fd)
        offset_a = token(fd, handle_a)
        repeat_a = token(fd, handle_a)
        memory_a = mapped(fd, offset_a)
        memory_a[:128] = bytes([0xa5]) * 128
        ioctl(fd, 0x09, struct.pack('<II', handle_a, 0))
        closed_handle_rejected = rejected(fd, offset_a)

        handle_b = create(fd)
        offset_b = token(fd, handle_b)
        memory_b = mapped(fd, offset_b)
        memory_b[:128] = bytes([0xb6]) * 128
        reused_slot_rejected = rejected(fd, offset_a)
        retained_after_slot_reuse = memory_a[:128] == bytes([0xa5]) * 128
        assert memory_b[:128] == bytes([0xb6]) * 128
        os.close(fd)
        fd = -1
        retained_after_fd_close = (memory_a[:128] == bytes([0xa5]) * 128
                                   and memory_b[:128] == bytes([0xb6]) * 128)

        reopened = os.open('/dev/dri/renderD128', os.O_RDWR)
        handle_c = create(reopened)
        offset_c = token(reopened, handle_c)
        reopened_file_rejected = rejected(reopened, offset_a) and rejected(reopened, offset_b)
        unissued_rejected = rejected(reopened, 0)

        child = os.fork()
        if child == 0:
            try:
                if memory_a[:128] != bytes([0xa5]) * 128 or memory_b[:128] != bytes([0xb6]) * 128:
                    os._exit(2)
                memory_a[16] = 0xc9
                os._exit(0)
            except BaseException:
                os._exit(3)
        _, status = os.waitpid(child, 0)
        shared_fork_pass = status == 0 and memory_a[16] == 0xc9 and memory_b[16] == 0xb6

        # The first fork proves sharing. A separate fork checks lifetime: the
        # child must retain both mappings after the parent releases its VMAs.
        read_end, write_end = os.pipe()
        try:
            child = os.fork()
            if child == 0:
                try:
                    os.close(write_end)
                    if os.read(read_end, 1) != b'U':
                        os._exit(4)
                    expected_a = bytearray([0xa5]) * 128
                    expected_a[16] = 0xc9
                    if memory_a[:128] != expected_a or memory_b[:128] != bytes([0xb6]) * 128:
                        os._exit(5)
                    memory_b[17] = 0xda
                    if memory_b[17] != 0xda:
                        os._exit(6)
                    memory_a.close()
                    memory_b.close()
                    os._exit(0)
                except BaseException:
                    os._exit(7)
            os.close(read_end)
            read_end = -1
            memory_a.close()
            memory_a = None
            memory_b.close()
            memory_b = None
            os.write(write_end, b'U')
            os.close(write_end)
            write_end = -1
            _, status = os.waitpid(child, 0)
            child_after_parent_unmap_pass = status == 0
        finally:
            if read_end >= 0:
                os.close(read_end)
            if write_end >= 0:
                os.close(write_end)
        cases = {'repeat_map_token_stable': repeat_a == offset_a,
                 'gem_slot_reused': handle_b == handle_a,
                 'closed_handle_token_rejected': closed_handle_rejected,
                 'slot_reuse_token_rejected': reused_slot_rejected,
                 'reopened_file_old_tokens_rejected': reopened_file_rejected,
                 'unissued_token_rejected': unissued_rejected,
                 'tokens_distinct': len({offset_a, offset_b, offset_c}) == 3,
                 'retained_mapping_after_slot_reuse': retained_after_slot_reuse,
                 'retained_mapping_after_fd_close': retained_after_fd_close,
                 'shared_mapping_fork_pass': shared_fork_pass,
                 'child_mapping_after_parent_unmap': child_after_parent_unmap_pass}
        print('GPU_MMAP_TOKEN_RESULT', json.dumps({'pass': all(cases.values()),
              'tokens': [offset_a, offset_b, offset_c], 'cases': cases}), flush=True)
        return int(not all(cases.values()))
    finally:
        if memory_a is not None:
            memory_a.close()
        if memory_b is not None:
            memory_b.close()
        if reopened >= 0:
            os.close(reopened)
        if fd >= 0:
            os.close(fd)


if __name__ == '__main__':
    raise SystemExit(main())
