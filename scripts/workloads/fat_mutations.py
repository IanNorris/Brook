#!/usr/bin/env python3
"""Private FAT mutation errno/compatibility controls; no atomic-replace claim."""
import errno
import os
from pathlib import Path
import re
import sys


def denied(error, operation):
    try:
        operation()
    except OSError as e:
        assert e.errno == error, (e.errno, error)
    else:
        raise AssertionError('unexpected success')


def main():
    assert len(sys.argv) == 2 and re.fullmatch('[0-9a-f]{16}', sys.argv[1])
    root = Path('/tmp')/('brook-fat-'+sys.argv[1]); root.mkdir()
    held = root/'held'; held_fd = os.open(held, os.O_CREAT | os.O_RDWR, 0o600)
    checks = 0
    try:
        denied(errno.EEXIST, lambda: root.mkdir()); checks += 1
        source = root/'source'; source.write_bytes(b'source bytes')
        target = root/'target'; target.write_bytes(b'old target bytes')
        source.rename(target)
        assert target.read_bytes() == b'source bytes' and not source.exists(); checks += 1
        denied(errno.ENOENT, lambda: source.rename(root/'absent')); checks += 1
        target.unlink(); denied(errno.ENOENT, target.unlink); checks += 1
        denied(errno.EBUSY, held.unlink); checks += 1
        denied(errno.EBUSY, lambda: held.rename(root/'moved-held')); checks += 1
        folder = root/'folder'; folder.mkdir()
        moved = root/'moved-folder'; folder.rename(moved); assert moved.is_dir(); checks += 1
        moved.rmdir()
        denied(errno.ENOENT, folder.rmdir); checks += 1
        denied(errno.ENOENT, lambda: (root/'absent-parent'/'child').mkdir()); checks += 1
        # Existing /boot-prefix compatibility is retained for true ENOENT only.
        contents = list(root.iterdir())
        assert len(contents) == 1 and contents[0].name.lower() == 'held'; checks += 1
    finally:
        os.close(held_fd)
        for p in root.iterdir():
            if p.is_dir(): p.rmdir()
            else: p.unlink()
        root.rmdir(); os.sync()
    print('FAT_MUTATIONS_PASS', checks, flush=True)


if __name__ == '__main__': main()
