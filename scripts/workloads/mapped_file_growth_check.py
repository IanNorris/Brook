#!/usr/bin/env python3
"""Run on Linux or Brook: grow a file while aliases in two processes remain live."""
import argparse
import ctypes
import json
import mmap
import os
from pathlib import Path
import tempfile


def check(directory):
    with tempfile.TemporaryDirectory(prefix='mapped-growth-', dir=directory) as tmp:
        path = Path(tmp) / 'backing'
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        readonly = None
        shared = reader = expanded = None
        child = None
        wake_read, wake_write = os.pipe()
        try:
            os.ftruncate(fd, 8192)
            readonly = os.open(path, os.O_RDONLY)
            shared = mmap.mmap(fd, 8192, access=mmap.ACCESS_WRITE)
            reader = mmap.mmap(readonly, 8192, access=mmap.ACCESS_READ)
            pattern = bytes(range(256)) * 32
            shared[:] = pattern
            child = os.fork()
            if child == 0:
                os.close(wake_write)
                try:
                    assert os.read(wake_read, 1) == b'G'
                    assert reader[:] == pattern and shared[:] == pattern
                    with mmap.mmap(fd, 270336, access=mmap.ACCESS_WRITE) as full:
                        assert full[8192:] == bytes(270336 - 8192)
                        full[8192:8196] = b'GROW'
                    os._exit(0)
                except BaseException:
                    os._exit(2)
            os.close(wake_read)
            wake_read = None
            # Both processes still own the original mappings at this point.
            os.ftruncate(fd, 270336)
            assert os.fstat(fd).st_size == 270336
            assert shared[:] == pattern and reader[:] == pattern
            assert os.pread(fd, 8192, 0) == pattern
            os.write(wake_write, b'G')
            waited, status = os.waitpid(child, 0)
            child = None
            assert waited > 0 and os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0
            expanded = mmap.mmap(readonly, 270336, access=mmap.ACCESS_READ)
            assert expanded[:8192] == pattern
            assert expanded[8192:8196] == b'GROW'
            assert expanded[8196:] == bytes(270336 - 8196)
            assert os.pread(fd, 4, 8192) == b'GROW'
            os.fsync(fd)
            for mapping in (shared, reader, expanded):
                mapping.close()
            shared = reader = expanded = None
            with path.open('rb') as backing:
                assert backing.read() == pattern + b'GROW' + bytes(270336 - 8196)
        finally:
            os.close(wake_write)
            if wake_read is not None:
                os.close(wake_read)
            if child is not None:
                os.waitpid(child, 0)
            for mapping in (shared, reader, expanded):
                if mapping is not None:
                    mapping.close()
            if readonly is not None:
                os.close(readonly)
            os.close(fd)
    print(json.dumps(dict(mapped_growth=True, directory=str(directory),
                          old_bytes=8192, new_bytes=270336,
                          cross_process_aliases=True, durable_contents=True)), flush=True)


def partial_tail(directory):
    with tempfile.TemporaryFile(dir=directory) as backing:
        backing.truncate(2048)
        shared = mmap.mmap(backing.fileno(), 2048, access=mmap.ACCESS_WRITE)
        private = mmap.mmap(backing.fileno(), 2048, access=mmap.ACCESS_COPY)
        try:
            shared[0] = 65
            private[0] = 66  # Force a private CoW page before growth.
            shared_address = ctypes.addressof(ctypes.c_char.from_buffer(shared))
            private_address = ctypes.addressof(ctypes.c_char.from_buffer(private))
            # The OS maps a full page for this partial file page. Probe its EOF
            # tail directly; Python's logical buffer bounds stop at 2048.
            shared_tail = ctypes.c_ubyte.from_address(shared_address + 3000)
            private_tail = ctypes.c_ubyte.from_address(private_address + 3000)
            shared_tail.value = 123
            private_tail.value = 124
            os.ftruncate(backing.fileno(), 2600)
            assert shared[0] == 65 and private[0] == 66
            assert shared_tail.value == 0 and private_tail.value == 124
            shared_tail.value = 125
            os.ftruncate(backing.fileno(), 57344)
            assert shared_tail.value == 0 and private_tail.value == 124
            assert os.pread(backing.fileno(), 1, 0) == b'A'
            assert os.pread(backing.fileno(), 4096 - 2048, 2048) == bytes(4096 - 2048)
            os.fsync(backing.fileno())
        finally:
            shared.close()
            private.close()
    print(json.dumps(dict(partial_mapped_growth=True, directory=str(directory),
                          shared_tail_zero=True, private_copy_preserved=True)), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    args = parser.parse_args()
    check(args.directory)
    partial_tail(args.directory)
