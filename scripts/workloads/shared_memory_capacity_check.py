#!/usr/bin/env python3
"""Exercise three large shared mappings, including their partial final pages."""
import argparse
import json
import mmap
import os
from pathlib import Path
import signal
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path('/dev/shm'))
    args = parser.parse_args()
    length = 25 * 1024 * 1024 + 16
    started = time.monotonic()
    descriptors = []
    child = None
    with tempfile.TemporaryDirectory(prefix='brook-shm-capacity-', dir=args.directory) as directory:
        try:
            for index in range(3):
                fd = os.open(Path(directory) / str(index), os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
                descriptors.append(fd)
                os.ftruncate(fd, length)
            child = os.fork()
            if child == 0:
                signal.alarm(90)
                try:
                    views = [mmap.mmap(fd, length) for fd in descriptors]
                    for index, view in enumerate(views):
                        for offset in range(0, length, 4096):
                            view[offset] = index + 1
                        view[-1] = index + 17
                    for view in views:
                        view.close()
                except BaseException:
                    os._exit(1)
                os._exit(0)
            _, status = os.waitpid(child, 0)
            child = None
            assert status == 0, dict(child_wait_status=status)
            for index, fd in enumerate(descriptors):
                assert os.pread(fd, 1, 0) == bytes([index + 1])
                assert os.pread(fd, 1, length - 1) == bytes([index + 17])
        finally:
            if child is not None:
                os.kill(child, signal.SIGKILL)
                os.waitpid(child, 0)
            for fd in descriptors:
                os.close(fd)
    print(json.dumps(dict(passed=True, files=3, bytes_per_file=length,
                          logical_bytes=length * 3, elapsed_s=time.monotonic() - started)), flush=True)


if __name__ == '__main__':
    main()
