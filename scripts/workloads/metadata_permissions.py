#!/usr/bin/env python3
"""Private chmod/utimens permission probes; root guest matrix or host owner controls.

Does not test directory traversal, capabilities or account isolation. Nix's
explicit timestamp bypass is excluded: run the full matrix on /data only.
"""
import argparse
import ctypes
import errno
import json
import os
from pathlib import Path
import re
import shutil
import stat

NOW, OMIT = (1 << 30) - 1, (1 << 30) - 2
OWNER, GROUP, OTHER = 66536, 131172, 66537
OLD = 1000000000
libc = ctypes.CDLL(None, use_errno=True)


class Timespec(ctypes.Structure):
    _fields_ = [('sec', ctypes.c_long), ('nsec', ctypes.c_long)]


def times(path, values, fd=-100):
    pair = None if values is None else (Timespec * 2)(*(Timespec(*v) for v in values))
    result = libc.syscall(ctypes.c_long(280), ctypes.c_int(fd),
                          ctypes.c_char_p(os.fsencode(path) if path is not None else None), pair,
                          ctypes.c_int(0))
    if result < 0:
        raise OSError(ctypes.get_errno(), 'utimensat')


def attempt(operation):
    try:
        return operation() or 0
    except OSError as e:
        return e.errno


def child(uid, gid, groups, operation):
    read, write = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(read)
        try:
            os.setgroups(groups); os.setgid(gid); os.setuid(uid)
            value = attempt(operation)
            os.write(write, json.dumps(value).encode())
            os._exit(0)
        except BaseException:
            os._exit(1)
    os.close(write)
    data = b''
    while chunk := os.read(read, 4096):
        data += chunk
    os.close(read)
    assert os.waitpid(pid, 0) == (pid, 0), 'probe child failed'
    return json.loads(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', default='/data')
    parser.add_argument('--case', required=True)
    parser.add_argument('--record-baseline', action='store_true')
    parser.add_argument('--host-owner-controls', action='store_true')
    args = parser.parse_args()
    assert re.fullmatch('[0-9a-f]{16}', args.case)
    assert args.host_owner_controls or os.geteuid() == 0
    directory = Path(args.root)/('brook-metadata-'+args.case)
    directory.mkdir(mode=0o777); directory.chmod(0o777)
    failures = []
    def check(name, observed, expected):
        print(json.dumps({'case': name, 'observed': observed, 'expected': expected}), flush=True)
        if observed != expected:
            failures.append(name)
    try:
        path = directory/'file'; path.write_bytes(b'metadata permission probe\n')
        if args.host_owner_controls:
            for group in sorted(set([os.getegid(), *os.getgroups()])):
                try:
                    os.chown(path, -1, group)
                except OSError as e:
                    if e.errno != errno.EINVAL:
                        raise
                    print('HOST_UNMAPPED_GROUP_SKIPPED', group, flush=True)
                    continue
                os.chmod(path, 0o2640)
                check('owner-setgid-group-'+str(group), stat.S_IMODE(path.stat().st_mode), 0o2640)
            path.chmod(0)
            check('owner-explicit-times', attempt(lambda: times(path, [(OLD, 0)]*2)), 0)
            check('owner-now-without-write', attempt(lambda: times(path, None)), 0)
        else:
            os.chown(path, OWNER, GROUP)
            for kind in ('path', 'fd', 'directory'):
                target = path
                if kind == 'directory':
                    target = directory/'dir'; target.mkdir(); os.chown(target, OWNER, GROUP)
                fd = os.open(target, os.O_RDONLY)
                try:
                    for label, gid, groups in [('primary', GROUP, []), ('supplementary', 100, [GROUP]), ('unrelated', 100, [])]:
                        target.chmod(0o640)
                        op = (lambda: os.fchmod(fd, 0o2640)) if kind == 'fd' else (lambda: os.chmod(target, 0o2640))
                        check(kind+'-chmod-'+label, child(OWNER, gid, groups, op), 0)
                        check(kind+'-setgid-'+label, stat.S_IMODE(target.stat().st_mode), 0o640 if label == 'unrelated' else 0o2640)
                    check(kind+'-foreign-chmod', child(OTHER, GROUP, [], lambda: os.chmod(target, 0o777)), errno.EPERM)
                finally:
                    os.close(fd)
            fd = os.open(path, os.O_RDONLY)
            cases = [
                ('owner-now', OWNER, 100, [], 0, None, 0, False),
                ('owner-explicit', OWNER, 100, [], 0, [(OLD+1, 0)]*2, 0, False),
                ('primary-now', OTHER, GROUP, [], 0o020, None, 0, False),
                ('supplementary-now', OTHER, 100, [GROUP], 0o020, [(0, NOW)]*2, 0, False),
                ('supplementary-fd-now', OTHER, 100, [GROUP], 0o020, None, 0, True),
                ('unrelated-denied-now', OTHER, 100, [], 0o020, None, errno.EACCES, False),
                ('other-write-now', OTHER, 100, [], 0o002, None, 0, False),
                ('group-no-other-fallback', OTHER, GROUP, [], 0o002, None, errno.EACCES, False),
                ('supplementary-no-other-fallback', OTHER, 100, [GROUP], 0o002, None, errno.EACCES, False),
                ('group-explicit-denied', OTHER, GROUP, [], 0o020, [(OLD+1, 0)]*2, errno.EPERM, False),
                ('group-mixed-denied', OTHER, GROUP, [], 0o020, [(0, NOW), (0, OMIT)], errno.EPERM, False),
                ('both-omit', OTHER, 100, [], 0, [(0, OMIT)]*2, 0, False),
            ]
            try:
                for name, uid, gid, groups, mode, values, error, descriptor in cases:
                    path.chmod(mode); os.utime(path, (OLD, OLD))
                    observed = child(uid, gid, groups, lambda: times(None if descriptor else path, values, fd if descriptor else -100))
                    check(name, observed, error)
                    if observed == 0 and error == 0:
                        mtime = int(path.stat().st_mtime)
                        check(name+'-mtime', mtime if name in ('owner-explicit', 'both-omit') else mtime != OLD,
                              OLD+1 if name == 'owner-explicit' else OLD if name == 'both-omit' else True)
                    elif observed != 0:
                        check(name+'-unchanged', int(path.stat().st_mtime), OLD)
            finally:
                os.close(fd)
    finally:
        shutil.rmtree(directory)
        os.sync()
    print('METADATA_FAILURES', json.dumps(failures), flush=True)
    if args.record_baseline:
        assert failures, 'negative control unexpectedly passed'
    else:
        assert not failures, failures
    print('METADATA_BASELINE_RECORDED' if args.record_baseline else 'METADATA_PERMISSIONS_PASS', flush=True)


if __name__ == '__main__':
    main()
