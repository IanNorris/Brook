#!/usr/bin/env python3
"""Existing-file open authorization matrix in a private fixture.

Run as root in Brook or an isolated Linux guest. Never changes the parent
identity. This checks leaf read/write authorization, not path/account isolation.
Optional arguments: result JSON path and fixture parent (default /data).
"""
import errno
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile


def probe(directory, uid, gid, groups):
    os.setgroups(groups)
    os.setresgid(gid, gid, gid)
    os.setresuid(uid, uid, uid)
    rows = []
    for path in sorted(directory.glob('mode-*')):
        mode = int(path.name[5:], 8)
        bits = 7 if uid == 0 else ((mode >> 6) & 7 if uid == 1000 else
                                 (mode >> 3) & 7 if gid == 200 or 200 in groups else mode & 7)
        for name, flags, required in [('read', os.O_RDONLY, 4), ('write', os.O_WRONLY, 2),
                                       ('readwrite', os.O_RDWR, 6),
                                       ('existing-create', os.O_RDWR | os.O_CREAT, 6),
                                       ('truncate', os.O_WRONLY | os.O_TRUNC, 2),
                                       ('readonly-truncate', os.O_RDONLY | os.O_TRUNC, 6),
                                       ('readwrite-truncate', os.O_RDWR | os.O_TRUNC, 6)]:
            expected = 0 if bits & required == required else errno.EACCES
            try:
                fd = os.open(path, flags, 0o600)
                os.close(fd)
                actual = 0
            except OSError as exc:
                actual = exc.errno
            rows.append(dict(mode=oct(mode), operation=name, expected=expected,
                             actual=actual, passed=actual == expected))
    # Creation grants the returned descriptor even when the new mode is 000.
    path = directory / ('created-%d' % uid)
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0)
    try:
        assert os.write(fd, b'new descriptor') == 14
        os.lseek(fd, 0, os.SEEK_SET)
        assert os.read(fd, 14) == b'new descriptor'
        os.fsync(fd)
    finally:
        os.close(fd)
    os.unlink(path)
    return rows


def main():
    if os.geteuid() != 0:
        raise SystemExit('Run this probe as root inside the test guest.')
    directory = Path(tempfile.mkdtemp(prefix='brook-open-permissions-', dir=sys.argv[2] if len(sys.argv)>2 else '/data'))
    results = {}
    try:
        os.chmod(directory, 0o777)
        for mode in [0o000, 0o200, 0o400, 0o600, 0o020, 0o040, 0o060,
                     0o002, 0o004, 0o006, 0o066, 0o606, 0o660, 0o666]:
            path = directory / ('mode-%03o' % mode)
            path.write_bytes(b'private permission fixture')
            os.chown(path, 1000, 200)
            os.chmod(path, mode)
        for name, uid, gid, groups in [('owner',1000,100,[]),
                                     ('primary-group',1001,200,[]),
                                     ('supplementary-group',1001,100,[200]),
                                     ('supplementary-mixed',1001,100,[900,200,50,200]),
                                     ('owner-with-groups',1000,100,[200]),
                                     ('unrelated',1002,100,[]),
                                     ('root',0,0,[])]:
            r,w = os.pipe()
            pid = os.fork()
            if pid == 0:
                os.close(r)
                try:
                    rows = probe(directory,uid,gid,groups)
                    payload = dict(rows=rows,passed=all(row['passed'] for row in rows))
                except BaseException as exc:
                    payload = dict(passed=False,error=repr(exc))
                data = json.dumps(payload).encode()
                while data:
                    data = data[os.write(w,data):]
                os.close(w)
                os._exit(0)
            os.close(w)
            data = bytearray()
            while chunk := os.read(r,65536): data.extend(chunk)
            os.close(r)
            _,status = os.waitpid(pid,0)
            assert status == 0,(name,status)
            results[name] = json.loads(data)
            failures = [row for row in results[name].get('rows',[]) if not row['passed']]
            print('PERMISSION_CASE',name,'PASS' if results[name]['passed'] else 'FAIL',
                  'failures=%d' % len(failures),
                  json.dumps(failures[:5] or results[name].get('error')),flush=True)
    finally:
        shutil.rmtree(directory)
    if len(sys.argv)>1:
        Path(sys.argv[1]).write_text(json.dumps(results,indent=2)+'\n')
    assert all(item['passed'] for item in results.values()),'open authorization mismatch'
    print('PERMISSIONS_PASS',sum(len(item['rows']) for item in results.values()),flush=True)

if __name__ == '__main__': main()
