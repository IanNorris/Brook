#!/usr/bin/env python3
"""Check ownership of our proc entries; exports counts, never proc contents."""
import ctypes
import json
import os
import sys
import threading


def paths():
    pid = str(os.getpid())
    return ['/proc/self', '/proc/' + pid, '/proc/self/fd',
            '/proc/self/task', '/proc/self/task/' + pid,
            '/proc/self/stat', '/proc/self/status', '/proc/self/cmdline']


def check(path):
    expected = (os.geteuid(), os.getegid())
    info = os.stat(path)
    assert (info.st_uid, info.st_gid) == expected, (path, 'stat')
    fd = os.open(path, os.O_RDONLY)
    try:
        opened = os.fstat(fd)
        assert (opened.st_uid, opened.st_gid) == expected, (path, 'fstat')
        assert (opened.st_dev, opened.st_ino) == (info.st_dev, info.st_ino)
    finally:
        os.close(fd)


def main():
    for path in paths():
        check(path)
    errors = []

    def worker():
        try:
            check('/proc/thread-self')
            check('/proc/thread-self/status')
        except BaseException as error:
            errors.append(error)

    thread = threading.Thread(target=worker)
    thread.start(); thread.join()
    assert not errors, errors

    parent = os.getpid()
    child = os.fork()
    if not child:
        try:
            check('/proc/' + str(parent))
            check('/proc/' + str(parent) + '/fd')
        except BaseException:
            os._exit(1)
        os._exit(0)
    _, status = os.waitpid(child, 0)
    assert status == 0

    # Opened ordinary proc files retain identity after the target is reaped.
    ready, release = os.pipe()
    child = os.fork()
    if not child:
        os.close(release)
        os.read(ready, 1)
        os._exit(0)
    os.close(ready)
    retained = []
    try:
        for name in ('status', 'stat', 'cmdline'):
            fd = os.open('/proc/' + str(child) + '/' + name, os.O_RDONLY)
            info = os.fstat(fd)
            retained.append((fd, (info.st_dev, info.st_ino, info.st_uid, info.st_gid)))
    finally:
        os.close(release)
        _, status = os.waitpid(child, 0)
        assert status == 0
    try:
        for fd, identity in retained:
            info = os.fstat(fd)
            assert (info.st_dev, info.st_ino, info.st_uid, info.st_gid) == identity
    finally:
        for fd, _ in retained:
            os.close(fd)

    effective_change = '--effective-change' in sys.argv
    if effective_change:
        assert os.geteuid() == 0
        original = (os.geteuid(), os.getegid())
        retained = [os.open(path, os.O_RDONLY) for path in paths()]
        try:
            os.setegid(1001); os.seteuid(1002)
            # Linux resets dumpability on an ID change. Restore it for this
            # ownership comparison; this does not test Brook dumpability.
            libc = ctypes.CDLL(None, use_errno=True)
            assert libc.prctl(4, 1, 0, 0, 0) == 0
            for path in paths():
                check(path)
            for fd in retained:
                info = os.fstat(fd)
                assert (info.st_uid, info.st_gid) == (1002, 1001)
        finally:
            os.seteuid(original[0]); os.setegid(original[1])
            for fd in retained:
                os.close(fd)
    print(json.dumps(dict(proc_ownership_pass=True, paths=8,
                          thread_aliases=2, foreign_same_owner=True,
                          retained_reaped_files=3,
                          effective_change=effective_change)), flush=True)


if __name__ == '__main__':
    main()
