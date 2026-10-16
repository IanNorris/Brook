#!/usr/bin/env python3
"""Check the persistent desktop home as root or the compatibility UID 1000.

Run write, perform a checked VM shutdown/reboot, then run verify as the same UID.
This checks storage and environment, not multi-user isolation.
"""
import os
from pathlib import Path
import pwd
import sys

home = Path(os.environ['HOME'])
assert home == Path('/data/home'), home
assert home.is_dir() and home.stat().st_uid == 1000
for variable, suffix in [('XDG_CONFIG_HOME', '.config'), ('XDG_CACHE_HOME', '.cache'),
                         ('XDG_DATA_HOME', '.local/share')]:
    directory = Path(os.environ[variable])
    assert directory == home / suffix and directory.is_dir(), variable
    assert directory.stat().st_uid == 1000, variable
if os.geteuid() == 0:
    assert pwd.getpwuid(0).pw_dir == str(home)
target = home / '.cache' / ('home-check-%d.bin' % os.geteuid())
expected = b'Brook persistent home\n' * 4096
if sys.argv[1] == 'write':
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'wb') as output:
        output.write(expected)
        output.flush()
        os.fsync(output.fileno())
elif sys.argv[1] != 'verify':
    raise SystemExit('usage: home.py write|verify')
assert target.read_bytes() == expected
assert target.stat().st_uid == os.geteuid()
print('PERSISTENT_HOME_%s_PASS uid=%d bytes=%d' % (sys.argv[1].upper(), os.geteuid(), len(expected)), flush=True)
