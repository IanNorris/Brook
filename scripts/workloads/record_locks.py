#!/usr/bin/env python3
"""Nonblocking process record locks: Linux control / Brook ext2 guest probe.

Usage: python3 record_locks.py DIRECTORY [PREPARED_HARDLINK_SOURCE ALIAS]
Only private temporary files are touched. Blocking waits and OFD locks are not
accepted by this probe. Fork children report assertions through a private pipe.
Optional aliases must already name the same inode; they are not deleted or
written. This permits testing inode identity before guest link creation exists.
"""
import ctypes
import errno
import json
import os
from pathlib import Path
import sys
import tempfile
import threading


class Flock(ctypes.Structure):
    _fields_ = [('type', ctypes.c_short), ('whence', ctypes.c_short),
                ('start', ctypes.c_longlong), ('length', ctypes.c_longlong),
                ('pid', ctypes.c_int)]


libc = ctypes.CDLL(None, use_errno=True)
libc.fcntl.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
assert ctypes.sizeof(Flock) == 32


def lock(fd, kind=1, start=0, length=100, command=6, whence=0):
    query = Flock(kind, whence, start, length, 0)
    ctypes.set_errno(0)
    result = libc.fcntl(fd, command, ctypes.byref(query))
    return result, ctypes.get_errno(), query


def acquire(fd, kind=1, start=0, length=100, whence=0):
    result, error, _ = lock(fd, kind, start, length, whence=whence)
    assert result == 0, (result, error)


def blocked(fd, kind=1, start=0, length=100):
    result, error, _ = lock(fd, kind, start, length)
    assert result == -1 and error in (errno.EAGAIN, errno.EACCES), (result, error)


def child(function):
    read, write = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(read)
        try:
            function()
            payload = {'passed': True}
        except BaseException as error:
            payload = {'passed': False, 'error': repr(error)}
        os.write(write, json.dumps(payload).encode())
        os._exit(0 if payload['passed'] else 1)
    os.close(write)
    pieces = []
    while True:
        data = os.read(read, 4096)
        if not data:
            break
        pieces.append(data)
    os.close(read)
    _, status = os.waitpid(pid, 0)
    assert status == 0, (status, b''.join(pieces))
    assert json.loads(b''.join(pieces))['passed']


def main():
    results = {}
    with tempfile.TemporaryDirectory(prefix='brook-record-locks-', dir=sys.argv[1]) as tmp:
        path = Path(tmp) / 'data'
        fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o600)
        os.write(fd, b'0' * 100)
        parent = os.getpid()

        def case(name, function):
            try:
                function()
                results[name] = {'passed': True}
            except BaseException as error:
                results[name] = {'passed': False, 'error': repr(error)}
            finally:
                acquire(fd, 2, 0, 0)
            print('RECORD_LOCK_CASE', name, json.dumps(results[name]), flush=True)

        def conflict_and_fork():
            acquire(fd)
            def check():
                result, error, found = lock(fd, command=5)
                assert result == 0 and found.type == 1 and found.pid == parent, (result, error, found.pid)
                assert found.start == 0 and found.length == 100
                blocked(fd)
                acquire(fd, 2)  # Child unlock cannot remove the parent's lock.
                blocked(fd)
            child(check)
        case('write_conflict_getlk_fork_owner', conflict_and_fork)

        def readers():
            acquire(fd, 0)
            def check():
                acquire(fd, 0)
                blocked(fd, 1)
                acquire(fd, 2)
            child(check)
        case('shared_readers_exclude_writer', readers)

        def split():
            acquire(fd)
            acquire(fd, 2, 40, 20)
            def check():
                acquire(fd, 1, 40, 20)
                blocked(fd, 1, 0, 40)
                blocked(fd, 1, 60, 40)
            child(check)
        case('partial_unlock_split', split)

        def conversion():
            acquire(fd, 0)
            acquire(fd, 1, 30, 40)
            def check():
                acquire(fd, 0, 0, 30)
                blocked(fd, 0, 30, 40)
                acquire(fd, 0, 70, 30)
            child(check)
        case('middle_read_to_write_conversion', conversion)

        def independent_close():
            acquire(fd)
            other = os.open(path, os.O_RDWR)
            os.close(other)
            child(lambda: acquire(fd))
        case('independent_open_close_releases', independent_close)

        def hardlink():
            prepared = len(sys.argv) == 4
            source = Path(sys.argv[2]) if prepared else path
            alias = Path(sys.argv[3]) if prepared else Path(tmp) / 'alias'
            if prepared:
                assert os.stat(source).st_ino == os.stat(alias).st_ino
                assert os.stat(source).st_dev == os.stat(alias).st_dev
            else:
                os.link(source, alias)
            source_fd = os.open(source, os.O_RDWR)
            try:
                acquire(source_fd)
                def check():
                    other = os.open(alias, os.O_RDWR)
                    try:
                        blocked(other)
                    finally:
                        os.close(other)
                child(check)
                # A child closing its alias must preserve the parent's locks.
                child(lambda: blocked(source_fd))
                other = os.open(alias, os.O_RDWR)
                os.close(other)
                child(lambda: acquire(source_fd))
            finally:
                os.close(source_fd)
                if not prepared:
                    alias.unlink()
        case('hardlink_identity_child_close', hardlink)

        def duplication():
            acquire(fd)
            os.dup2(fd, fd)
            child(lambda: blocked(fd))
            other = os.dup(fd)
            try:
                os.dup2(fd, other)  # Distinct descriptor, same inode: real close.
                child(lambda: acquire(fd))
            finally:
                os.close(other)
        case('dup2_same_fd_and_same_inode', duplication)

        def exit_release():
            child(lambda: acquire(fd))
            acquire(fd)
        case('owner_exit_releases', exit_release)

        def exec_owner(cloexec):
            ready_read, ready_write = os.pipe()
            continue_read, continue_write = os.pipe()
            pid = os.fork()
            if pid == 0:
                try:
                    os.close(ready_read)
                    os.close(continue_write)
                    os.set_inheritable(fd, not cloexec)
                    os.set_inheritable(ready_write, True)
                    os.set_inheritable(continue_read, True)
                    acquire(fd)
                    program = 'import os,sys;os.write(int(sys.argv[1]),b"R");os.read(int(sys.argv[2]),1)'
                    os.execv(sys.executable, [sys.executable, '-c', program,
                                             str(ready_write), str(continue_read)])
                except BaseException:
                    os.write(ready_write, b'E')
                    os._exit(1)
            os.close(ready_write)
            os.close(continue_read)
            try:
                assert os.read(ready_read, 1) == b'R'
                result, error, found = lock(fd, command=5)
                assert result == 0, error
                if cloexec:
                    assert found.type == 2, found.type
                else:
                    assert found.type == 1 and found.pid == pid, (found.type, found.pid, pid)
            finally:
                os.write(continue_write, b'C')
                os.close(continue_write)
                os.close(ready_read)
                _, status = os.waitpid(pid, 0)
            assert status == 0, status
        case('leader_exec_preserves', lambda: exec_owner(False))
        case('cloexec_releases', lambda: exec_owner(True))

        def threaded_owner():
            errors = []
            acquire(fd)
            def worker():
                try:
                    acquire(fd, 0)
                except BaseException as error:
                    errors.append(repr(error))
            thread = threading.Thread(target=worker)
            thread.start()
            thread.join()
            assert not errors, errors
            child(lambda: acquire(fd, 0))
        case('threads_share_owner', threaded_owner)

        def backwards_eof():
            os.lseek(fd, 30, os.SEEK_SET)
            acquire(fd, 1, -10, -5, 1)  # [15,19] relative to current offset.
            acquire(fd, 1, 20, 0, 2)  # [120,infinity] relative to EOF.
            def check():
                blocked(fd, 1, 15, 5)
                acquire(fd, 1, 20, 100)
                blocked(fd, 1, 10000, 1)
            child(check)
        case('seek_cur_end_negative_length_future_eof', backwards_eof)

        def descriptor_access():
            for mode, kind in ((os.O_RDONLY, 1), (os.O_WRONLY, 0), (3, 0), (3, 1)):
                other = os.open(path, mode)
                try:
                    result, error, _ = lock(other, kind)
                    assert result == -1 and error == errno.EBADF, (mode, kind, result, error)
                finally:
                    os.close(other)
        case('descriptor_access_modes', descriptor_access)

        def invalid_ranges():
            for whence, start, length, expected in ((3, 0, 1, errno.EINVAL),
                    (0, -1, 1, errno.EINVAL), (0, 0, -1, errno.EINVAL),
                    (0, 0x7fffffffffffffff, 2, errno.EOVERFLOW)):
                result, error, _ = lock(fd, 1, start, length, whence=whence)
                assert result == -1 and error == expected, (result, error, expected)
        case('invalid_ranges_and_overflow', invalid_ranges)
        os.close(fd)
    print('RECORD_LOCK_RESULT', json.dumps(results), flush=True)
    return 0 if all(result['passed'] for result in results.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
