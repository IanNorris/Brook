#!/usr/bin/env python3
"""Rootless mkdir/mkdirat error-ordering regression in an owned fixture."""
import errno
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile


def main(parent):
    assert os.geteuid() != 0, 'Run as an ordinary user to exercise permissions'
    root = Path(tempfile.mkdtemp(prefix='mkdir-permissions-', dir=parent))
    rows = []
    try:
        locked = root / 'parent'
        locked.mkdir()
        (locked / 'directory').mkdir()
        (locked / 'file').write_text('preserve me')
        os.symlink('directory', locked / 'link')
        os.symlink('absent', locked / 'dangling')
        fd = os.open(locked, os.O_RDONLY | os.O_DIRECTORY)
        try:
            for mode in (0o555, 0o444):
                os.chmod(locked, mode)
                for relative in (False, True):
                    for name in ('directory', 'directory/', 'file', 'file/',
                                 'link', 'link/', 'dangling', 'dangling/', 'missing'):
                        expected = errno.EACCES if mode == 0o444 or name == 'missing' else errno.EEXIST
                        try:
                            if relative:
                                os.mkdir(name, dir_fd=fd)
                            else:
                                os.mkdir(str(locked) + '/' + name)
                            actual = 0
                        except OSError as exc:
                            actual = exc.errno
                        row = dict(mode=oct(mode), mkdirat=relative, name=name,
                                   expected=expected, actual=actual)
                        rows.append(row)
                        print('MKDIR_CASE', json.dumps(row), flush=True)
        finally:
            os.close(fd)
            os.chmod(locked, 0o755)
        assert (locked / 'file').read_text() == 'preserve me'
        assert sorted(p.name for p in locked.iterdir()) == ['dangling', 'directory', 'file', 'link']
        failures = [row for row in rows if row['actual'] != row['expected']]
        print('MKDIR_RESULT', json.dumps(dict(cases=len(rows), failures=failures)), flush=True)
        return bool(failures)
    finally:
        shutil.rmtree(root)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
