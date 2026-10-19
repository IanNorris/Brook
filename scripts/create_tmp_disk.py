#!/usr/bin/env python3
"""Create a private ext2 /tmp image with Unix ownership and sticky permissions.

Attach with BROOK_TMP_DISK=/absolute/path/to/tmp.img. The launcher creates
this disk by default, separately from /data, /nix and optional media disks.
The existing BROOK.MNT boot probe mounts it at /tmp. Contents persist until
the image is replaced; this is not tmpfs or automatic per-boot cleanup.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--size-mib', type=int, default=128)
    args = parser.parse_args()
    if args.size_mib < 16:
        parser.error('--size-mib must be at least 16')
    output = args.output.absolute()
    if output.exists() or output.is_symlink():
        parser.error('--output already exists')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='brook-tmp-', dir=output.parent) as name:
        work = Path(name)
        tree = work / 'tree'
        tree.mkdir()
        (tree / 'BROOK.MNT').write_text('/tmp\n')
        image = work / 'tmp.img'
        with image.open('xb') as stream:
            stream.truncate(args.size_mib * 1024**2)
        subprocess.run(['mkfs.ext2', '-q', '-F', '-b', '4096', '-L', 'BROOKTMP',
                        '-d', str(tree), str(image)], check=True)
        for target, mode in [('/', '041777'), ('/BROOK.MNT', '0100444')]:
            for field, value in [('uid', '0'), ('gid', '0'), ('mode', mode)]:
                subprocess.run(['debugfs', '-w', '-R',
                                f'set_inode_field {target} {field} {value}', str(image)],
                               check=True, capture_output=True)
        stat = subprocess.run(['debugfs', '-R', 'stat /', str(image)],
                              check=True, capture_output=True, text=True).stdout
        if not re.search(r'Mode:\s+01777\b', stat) or not re.search(r'User:\s+0\s+Group:\s+0\b', stat):
            raise RuntimeError('Temporary disk root metadata verification failed: ' + stat)
        subprocess.run(['e2fsck', '-fn', str(image)], check=True)
        # Publish without overwriting an image created by another invocation.
        os.link(image, output)
    print(json.dumps({'image': str(output), 'mount': '/tmp', 'uid': 0, 'gid': 0,
                      'mode': '01777', 'size_mib': args.size_mib,
                      'contents_persist': True}))


if __name__ == '__main__':
    main()
