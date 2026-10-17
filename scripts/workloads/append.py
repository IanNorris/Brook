#!/usr/bin/env python3
"""Private append/manifest regression on Linux or Brook.

Arguments: result JSON, fixture parent. Does not modify the real Nix manifest.
"""
import ctypes
import errno
import fcntl
import json
import os
from pathlib import Path
import sys
import tempfile

results = {}
directory = Path(tempfile.mkdtemp(prefix='brook-append-', dir=sys.argv[2] if len(sys.argv)>2 else '/tmp'))


def case(name, fn):
    try:
        detail = fn()
        results[name] = dict(passed=True, detail=detail)
    except BaseException as exc:
        results[name] = dict(passed=False, error=repr(exc))
    print('APPEND_CASE', name, json.dumps(results[name]), flush=True)


def basic():
    path = directory / 'basic'
    path.write_bytes(b'sentinel\n')
    fd = os.open(path, os.O_WRONLY | os.O_APPEND)
    other = os.open(path, os.O_WRONLY | os.O_APPEND)
    try:
        os.lseek(fd, 0, 0)
        assert os.write(fd, b'first\n') == 6
        os.lseek(other, 1, 0)
        assert os.write(other, b'second\n') == 7
        assert path.read_bytes() == b'sentinel\nfirst\nsecond\n'
        assert os.lseek(fd, 0, 1) == 15
        before = os.lseek(other, 0, 1)
        assert os.pwrite(other, b'positioned\n', 0) == 11
        assert os.lseek(other, 0, 1) == before
        assert path.read_bytes().endswith(b'second\npositioned\n')
        os.lseek(fd, 0, 0)
        assert os.write(fd, b'') == 0 and os.lseek(fd, 0, 1) == 0
        os.fsync(fd)
    finally:
        os.close(fd); os.close(other)
    return 'fresh EOF across independent opens, seek, pwrite offset, zero write'


def flags():
    path = directory / 'flags'
    path.write_bytes(b'keep')
    fd = os.open(path, os.O_WRONLY)
    try:
        fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) | os.O_APPEND)
        assert os.write(fd, b'A') == 1
        fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) & ~os.O_APPEND)
        os.lseek(fd, 0, 0)
        assert os.write(fd, b'K') == 1
        assert path.read_bytes() == b'KeepA'
    finally: os.close(fd)
    fd = os.open(path, os.O_RDONLY | os.O_APPEND)
    try:
        for fn in [lambda: os.write(fd, b'x'), lambda: os.writev(fd, [b'x']),
                   lambda: os.pwrite(fd, b'x', 0)]:
            try: fn()
            except OSError as exc: assert exc.errno == errno.EBADF, exc
            else: raise AssertionError('readonly write accepted')
        assert path.read_bytes() == b'KeepA'
    finally: os.close(fd)
    return 'F_SETFL toggles append; readonly descriptors reject all write APIs'


def manifest():
    path = directory / 'manifest.tsv'
    rows = [b'chromium\t148.0.7778.96\tchromium-store\n',
            b'brook-codium\t1.112.01907\tcodium-store\n',
            b'firefox-stock\t150.0.1\tfirefox-store\n']
    libc = ctypes.CDLL(None, use_errno=True)
    libc.fopen.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    libc.fopen.restype = ctypes.c_void_p
    libc.fwrite.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t, ctypes.c_void_p]
    libc.fwrite.restype = ctypes.c_size_t
    libc.fclose.argtypes = [ctypes.c_void_p]
    for row in rows:
        f = libc.fopen(os.fsencode(path), b'a')
        assert f, ctypes.get_errno()
        assert libc.fwrite(row, 1, len(row), f) == len(row)
        assert libc.fclose(f) == 0
    assert path.read_bytes() == b''.join(rows), path.read_bytes()
    return 'libc fopen(a)/buffered fwrite/fclose preserve three package rows'


def concurrent(vectors):
    path = directory / ('writev-records' if vectors else 'write-records')
    path.write_bytes(b'')
    children = []
    for worker in range(4):
        pid = os.fork()
        if pid == 0:
            try:
                fd = os.open(path, os.O_WRONLY | os.O_APPEND)
                for n in range(40):
                    record = ('%d:%03d:' % (worker, n)).encode() + bytes([65+worker])*249 + b'\n'
                    assert len(record) == 256
                    os.lseek(fd, 0, 0)
                    written = os.writev(fd, [record[:7], record[7:128], record[128:]]) if vectors else os.write(fd, record)
                    assert written == len(record), written
                os.fsync(fd); os.close(fd); os._exit(0)
            except BaseException as exc:
                print('APPEND_CHILD_ERROR', repr(exc), flush=True); os._exit(1)
        children.append(pid)
    statuses = [os.waitpid(pid, 0)[1] for pid in children]
    assert statuses == [0]*4, statuses
    data = path.read_bytes()
    assert len(data) == 4*40*256, len(data)
    expected = {('%d:%03d:' % (w, n)).encode() + bytes([65+w])*249+b'\n' for w in range(4) for n in range(40)}
    records = [data[n:n+256] for n in range(0, len(data), 256)]
    assert len(records) == len(expected) and set(records) == expected
    return '160 complete unique records from four independent processes'


def retained():
    path = directory / 'retained'
    path.write_bytes(b'old')
    fd = os.open(path, os.O_WRONLY | os.O_APPEND)
    reader = os.open(path, os.O_RDONLY)
    try:
        moved = directory / 'renamed'
        path.rename(moved)
        assert os.write(fd, b'A') == 1
        os.unlink(moved)
        assert os.writev(fd, [b'B', b'C']) == 2
        assert os.read(reader, 20) == b'oldABC'
        os.fsync(fd)
    finally: os.close(fd); os.close(reader)
    return 'append after rename/unlink uses retained inode'


def large():
    path = directory / 'large'
    path.write_bytes(b'start')
    fd = os.open(path, os.O_WRONLY | os.O_APPEND)
    data = bytes(range(256))*1024 + b'end'
    try:
        position = 0
        while position < len(data):
            count = os.writev(fd, [data[position:position+39000], data[position+39000:]])
            assert 0 < count <= len(data)-position
            position += count
        os.fsync(fd)
    finally: os.close(fd)
    assert path.read_bytes() == b'start'+data
    return '262147-byte vectored payload survives bounded short-write retries'


try:
    for name, fn in [('basic', basic), ('flags', flags), ('manifest', manifest),
                     ('concurrent-write', lambda: concurrent(False)),
                     ('concurrent-writev', lambda: concurrent(True)),
                     ('retained', retained), ('large', large)]:
        case(name, fn)
finally:
    try:
        # Flat fixtures only. Avoid holding an open dirfd across FAT rmdir,
        # where retained directory entries deliberately return EBUSY.
        for path in list(directory.iterdir()): path.unlink()
        directory.rmdir()
    except OSError as exc:
        results['cleanup'] = dict(passed=False, error=repr(exc), fixture=str(directory))
    finally:
        if len(sys.argv)>1: Path(sys.argv[1]).write_text(json.dumps(results, indent=2)+'\n')
        print('APPEND_RESULT', json.dumps(results), flush=True)
if not all(row['passed'] for row in results.values()): raise SystemExit(1)
