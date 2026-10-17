#!/usr/bin/env python3
"""Rename metadata regression with deliberately old mtimes; private fixture only."""
import json
import os
from pathlib import Path
import sys
import tempfile

root = Path(tempfile.mkdtemp(prefix='brook-renametime-', dir=sys.argv[2] if len(sys.argv)>2 else '/tmp'))
results = {}


def remove(path):
    if path.is_symlink() or not path.is_dir(): path.unlink()
    else:
        for entry in list(path.iterdir()): remove(entry)
        path.rmdir()


def check(kind, cross, replace):
    label = '%s-%s%s' % (kind, 'cross' if cross else 'same', '-replace' if replace else '')
    base = root/label;base.mkdir()
    old_parent = base/'a';new_parent = base/'b' if cross else old_parent
    old_parent.mkdir()
    if cross: new_parent.mkdir()
    source, target = old_parent/'old', new_parent/'new'
    if kind == 'directory':
        source.mkdir();(source/'child').write_bytes(b'contents')
        if replace: target.mkdir()
    elif kind == 'file':
        source.write_bytes(b'contents')
        if replace: target.write_bytes(b'replace')
    else:
        source.symlink_to('relative-target')
        if replace: target.symlink_to('old-target')
    os.utime(source, ns=(1510000000000000000, 1520000000000000000), follow_symlinks=False)
    os.utime(old_parent, (1500000000, 1500000000))
    if cross: os.utime(new_parent, (1500000000, 1500000000))
    before = source.lstat()
    os.rename(source, target)
    after = target.lstat()
    assert before.st_mtime_ns == after.st_mtime_ns, ('mtime', before.st_mtime_ns, after.st_mtime_ns)
    for field in ('st_ino', 'st_uid', 'st_gid', 'st_mode', 'st_nlink'):
        assert getattr(before, field) == getattr(after, field), field
    assert after.st_ctime_ns >= before.st_ctime_ns
    assert not source.exists() and not source.is_symlink()
    assert old_parent.stat().st_mtime_ns > 1500000000000000000
    assert new_parent.stat().st_mtime_ns > 1500000000000000000
    if kind == 'directory':
        assert (target/'child').read_bytes() == b'contents'
        assert os.stat(str(target)+'/..').st_ino == new_parent.stat().st_ino
    elif kind == 'file': assert target.read_bytes() == b'contents'
    else: assert os.readlink(target) == 'relative-target'
    return label, {'inode': after.st_ino, 'mtime_ns': after.st_mtime_ns}


try:
    for kind in ('file', 'directory', 'symlink'):
        for cross, replace in ((False, False), (True, False), (True, True)):
            name = '%s-%s%s' % (kind, 'cross' if cross else 'same', '-replace' if replace else '')
            try:
                _, detail = check(kind, cross, replace)
                results[name] = dict(passed=True, detail=detail)
            except BaseException as exc: results[name] = dict(passed=False, error=repr(exc))
            print('RENAME_TIME_CASE', name, json.dumps(results[name]), flush=True)
finally:
    remove(root)
    if len(sys.argv)>1: Path(sys.argv[1]).write_text(json.dumps(results, indent=2)+'\n')
    print('RENAME_TIME_RESULT', json.dumps(results), flush=True)
if any(not r['passed'] for r in results.values()): raise SystemExit(1)
