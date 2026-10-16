#!/usr/bin/env python3
"""Root-only ownership regression across Brook's data and Nix mounts.

Uses only private temporary directories; removes them after checking. This is
not acceptance of a complete accounts/capabilities/permission implementation.
"""
import ctypes
import errno
import os
from pathlib import Path
import shutil
import stat
import tempfile

assert os.geteuid() == 0
directories = []


def owner(path, uid, gid, follow=True):
    st = os.stat(path, follow_symlinks=follow)
    assert (st.st_uid, st.st_gid) == (uid, gid), (str(path), st.st_uid, st.st_gid)


def error(expected, operation):
    try:
        operation()
    except OSError as exc:
        assert exc.errno == expected, (expected, exc.errno)
    else:
        raise AssertionError('ownership operation falsely reported success')


try:
    for root in ['/data', '/nix']:
        directory = Path(tempfile.mkdtemp(prefix='brook-ownership-', dir=root))
        directories.append(directory)
        payload = directory / 'payload'
        fd = os.open(payload, os.O_CREAT | os.O_RDWR, 0o600)
        try:
            os.write(fd, b'ownership survives writes')
            os.fchown(fd, 1000, 100)
            assert os.fstat(fd).st_uid == 1000
            os.write(fd, b' and fsync')
            os.fsync(fd)
            owner(payload, 1000, 100)
            os.chown(payload, 1001, -1)
            owner(payload, 1001, 100)
            os.chmod(payload, 0o6750)
            os.chown(payload, 1002, 101)
            assert stat.S_IMODE(payload.stat().st_mode) & 0o6000 == 0
            os.chown(payload, -1, -1)
            owner(payload, 1002, 101)
            cwd = os.getcwd()
            try:
                os.chdir(directory)
                os.chown('payload', 1003, 102)
            finally:
                os.chdir(cwd)
            owner(payload, 1003, 102)
            dfd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.chown('payload', 1004, 103, dir_fd=dfd)
            finally:
                os.close(dfd)
            owner(payload, 1004, 103)
            link = directory / 'link'
            os.symlink('payload', link)
            os.lchown(link, 1005, 104)
            owner(link, 1005, 104, False)
            owner(payload, 1004, 103)
            os.chown(link, 1006, 105)
            owner(payload, 1006, 105)
            owner(link, 1005, 104, False)
            error(errno.ENOENT, lambda: os.chown(directory / 'absent', 1000, 100))
            error(errno.EBADF, lambda: os.fchown(-1, 1000, 100))
            error(errno.ENOTDIR, lambda: os.chown('payload', 1000, 100, dir_fd=fd))
            error(errno.EOVERFLOW, lambda: os.chown(payload, 65536, 100))
            owner(payload, 1006, 105)
            libc = ctypes.CDLL(None, use_errno=True)
            libc.fchownat.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_int]
            assert libc.fchownat(fd, b'', 1008, 107, 0x1000) == 0, ctypes.get_errno()
            owner(payload, 1008, 107)
        finally:
            os.close(fd)
    cross = directories[0] / 'cross-mount-link'
    os.symlink(str(directories[1] / 'payload'), cross)
    os.chown(cross, 1009, 108)
    owner(directories[1] / 'payload', 1009, 108)
    owner(directories[0] / 'payload', 1008, 107)
    for directory in directories:
        payload = directory / 'payload'
        fd = os.open(payload, os.O_RDWR)
        try:
            os.unlink(payload)
            os.fchown(fd, 1010, 109)
            st = os.fstat(fd)
            assert (st.st_uid, st.st_gid, st.st_nlink) == (1010, 109, 0)
            os.write(fd, b'unlinked file still alive')
            os.fsync(fd)
            assert os.fstat(fd).st_uid == 1010
        finally:
            os.close(fd)
    print('OWNERSHIP_PATH_FD_SYMLINK_TWO_MOUNTS_PASS', flush=True)
finally:
    for directory in reversed(directories):
        shutil.rmtree(directory)
