#!/usr/bin/env python3
"""Bounded proc FD stat/close contention probe; use only in a test guest."""
import errno
import json
import os
import signal
import stat
import threading


def main():
    signal.alarm(60)
    gate = threading.Barrier(4)
    errors = []
    counts = [0] * 4

    def run(index):
        try:
            gate.wait()
            for _ in range(300):
                if index < 2:
                    fd = os.memfd_create('brook-fd-race')
                    os.ftruncate(fd, 8193)
                    os.close(fd)
                else:
                    for name in os.listdir('/proc/self/fd'):
                        if not name.isdecimal():
                            continue
                        try:
                            result = os.stat('/proc/self/fd/' + name)
                            if stat.S_ISLNK(result.st_mode):
                                raise AssertionError('followed stat returned a symlink')
                        except OSError as exc:
                            # Other threads and the directory iterator may close
                            # their descriptors between enumeration and stat.
                            if exc.errno not in (errno.ENOENT, errno.EBADF):
                                raise
                counts[index] += 1
        except BaseException as exc:
            errors.append(type(exc).__name__ + ':' + str(getattr(exc, 'errno', '')))

    workers = [threading.Thread(target=run, args=(i,)) for i in range(4)]
    for worker in workers:
        worker.start()
    for worker in workers:
        worker.join()
    signal.alarm(0)
    result = {'iterations': counts, 'errors': errors}
    print('PROC_FD_STAT_RACE', json.dumps(result), flush=True)
    return 0 if not errors and counts == [300] * 4 else 1


if __name__ == '__main__':
    raise SystemExit(main())
