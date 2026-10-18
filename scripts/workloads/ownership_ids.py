#!/usr/bin/env python3
"""Private ext2 UID/GID-32 fixtures. Root maintenance only; no account isolation claim."""
import argparse
import errno
import json
import os
from pathlib import Path
import re
import shutil
import stat
import traceback

UID, GID = 66536, 131172
UID2, GID2 = 263144, 196708
MAX_UID, MAX_GID = 0xfffffffe, 0xfffffffd
PAYLOAD = b'Brook full-width inode ownership\n'


def identity(path, uid, gid, follow=True):
    s = path.stat() if follow else path.lstat()
    assert (s.st_uid, s.st_gid) == (uid, gid), (str(path), s.st_uid, s.st_gid, uid, gid)
    return s


def denied(code, operation):
    try:
        result = operation()
    except OSError as e:
        assert e.errno == code, (e.errno, code)
        return
    if isinstance(result, int):
        os.close(result)
    raise AssertionError('operation unexpectedly allowed')


def as_user(uid, gid, operation, groups=()):
    pid = os.fork()
    if pid == 0:
        try:
            os.setgroups(list(groups)); os.setgid(gid); os.setuid(uid)
            assert os.geteuid() == uid and os.getegid() == gid
            operation()
            os._exit(0)
        except BaseException:
            traceback.print_exc(); os._exit(1)
    assert os.waitpid(pid, 0) == (pid, 0), ('child failure', uid, gid)


def create(directory, uid, gid):
    os.umask(0o077)
    p = directory/'high-file'
    fd = os.open(p, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.write(fd, PAYLOAD); os.fsync(fd); os.close(fd)
    (directory/'high-dir').mkdir(mode=0o700)
    os.symlink('high-file', directory/'short-link')
    os.symlink('missing-component/'*6 + 'target', directory/'long-link')


def verify(directory):
    assert identity(directory/'high-file', UID, GID).st_size == len(PAYLOAD)
    assert (directory/'high-file').read_bytes() == PAYLOAD
    identity(directory/'high-dir', UID, GID)
    identity(directory/'short-link', UID2, GID2, False)
    identity(directory/'long-link', UID, GID, False)
    identity(directory/'max-file', MAX_UID, MAX_GID)
    identity(directory/'changed', UID2, GID2)
    identity(directory/'brook-link', 1000, 100, False)
    assert os.readlink(directory/'short-link') == 'high-file'
    assert os.readlink(directory/'long-link') == 'missing-component/'*6 + 'target'


def prepare(directory, enforce):
    directory.mkdir(mode=0o777); directory.chmod(0o777)
    as_user(UID, GID, lambda: create(directory, UID, GID))
    if not enforce:
        observed = {p.name: (p.lstat().st_uid, p.lstat().st_gid)
                    for p in directory.iterdir()}
        print('BASELINE_OBSERVED', json.dumps(observed), flush=True)
        assert observed['high-file'] != (UID, GID)
        assert observed['short-link'] == (0, 0)
        return
    identity(directory/'high-file', UID, GID)
    identity(directory/'short-link', UID, GID, False)
    identity(directory/'long-link', UID, GID, False)
    as_user(1000, 100, lambda: os.symlink('high-file', directory/'brook-link'))
    identity(directory/'brook-link', 1000, 100, False)
    as_user(UID, GID, lambda: os.chmod(directory/'high-file', 0o600))
    as_user(UID, GID, lambda: os.utime(directory/'high-file', (1000000000, 1000000000)))
    as_user(1000, 100, lambda: denied(errno.EPERM, lambda: os.chmod(directory/'high-file', 0o600)))
    if directory.parent == Path('/data'):
        as_user(1000, 100, lambda: denied(errno.EACCES, lambda: os.open(directory/'high-file', os.O_RDONLY)))
    else:
        print('NIX_OPEN_PERMISSION_BYPASS_NOT_ACCEPTED_AS_ISOLATION', flush=True)
    def read_owned():
        assert (directory/'high-file').read_bytes() == PAYLOAD
    as_user(UID, GID, read_owned)
    maximum = directory/'max-file'; maximum.write_bytes(PAYLOAD)
    os.chown(maximum, MAX_UID, MAX_GID); identity(maximum, MAX_UID, MAX_GID)
    as_user(MAX_UID, MAX_GID, lambda: os.chmod(maximum, 0o600))
    changed = directory/'changed'; changed.write_bytes(PAYLOAD)
    fd = os.open(changed, os.O_RDWR)
    try:
        os.fchown(fd, UID, GID); identity(changed, UID, GID)
        os.chown(changed, -1, GID2); identity(changed, UID, GID2)
        os.chown(changed, 1000, 100); identity(changed, 1000, 100)
        os.chown(changed, UID2, GID2); identity(changed, UID2, GID2)
        assert os.fstat(fd).st_uid == UID2
        os.lchown(directory/'short-link', UID2, GID2)
        identity(directory/'high-file', UID, GID)
    finally:
        os.close(fd)
    group = directory/'group-file'; group.write_bytes(PAYLOAD)
    os.chown(group, 1000, GID); group.chmod(0o640)
    as_user(1001, GID, lambda: group.read_bytes())
    as_user(1001, 100, lambda: group.read_bytes(), groups=(GID,))
    if directory.parent == Path('/data'):
        as_user(1001, 100, lambda: denied(errno.EACCES, lambda: os.open(group, os.O_RDONLY)))
    unlinked = directory/'unlinked'; fd = os.open(unlinked, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    try:
        os.fchown(fd, UID, GID); unlinked.unlink(); os.fchown(fd, MAX_UID, MAX_GID)
        s = os.fstat(fd); assert (s.st_uid, s.st_gid, s.st_nlink) == (MAX_UID, MAX_GID, 0)
        os.write(fd, PAYLOAD); os.fsync(fd)
    finally:
        os.close(fd)
    verify(directory)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--phase', choices=['baseline', 'prepare', 'verify', 'cleanup'], required=True)
    parser.add_argument('--case', required=True)
    args = parser.parse_args()
    assert os.geteuid() == 0 and re.fullmatch('[0-9a-f]{16}', args.case)
    directories = [Path(root)/('brook-uid32-'+args.case) for root in ('/data', '/nix')]
    for directory in directories:
        if args.phase in ('baseline', 'prepare'):
            assert not directory.exists()
            prepare(directory, args.phase == 'prepare')
        elif args.phase == 'verify':
            verify(directory)
            as_user(UID, GID, lambda: identity(directory/'high-file', UID, GID))
            as_user(MAX_UID, MAX_GID, lambda: os.chmod(directory/'max-file', 0o600))
            as_user(1001, 100, lambda: (directory/'group-file').read_bytes(), groups=(GID,))
            as_user(1000, 100, lambda: denied(errno.EPERM, lambda: os.chmod(directory/'high-file', 0o600)))
            if directory.parent == Path('/data'):
                as_user(1000, 100, lambda: denied(errno.EACCES, lambda: os.open(directory/'high-file', os.O_RDONLY)))
                as_user(1001, 100, lambda: denied(errno.EACCES, lambda: os.open(directory/'group-file', os.O_RDONLY)))
        else:
            assert directory.is_dir() and not directory.is_symlink()
            shutil.rmtree(directory)
        print('UID32', args.phase, str(directory), 'PASS', flush=True)
    os.sync()
    print('UID32_'+args.phase.upper()+'_PASS', flush=True)


if __name__ == '__main__':
    main()
