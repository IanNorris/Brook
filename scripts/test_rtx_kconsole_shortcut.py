#!/usr/bin/env python3
"""Check that the RTX update file set carries the desktop Kernel Console shortcut."""

import argparse
import hashlib
from pathlib import Path
import subprocess

from rtx_smoke_update import load_set


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--files', required=True, type=Path)
    args = parser.parse_args()
    _, manifest = load_set(args.files)
    assert manifest['session'] == 'desktop', 'expected an RTX desktop file set'
    shortcut = 'BOOT/SHORTCUTS/KCONSOLE.RC'
    expected = (Path(__file__).resolve().parents[1] / 'data/shortcuts/kernel_console.rc').read_bytes()
    actual = subprocess.check_output(['mtype', '-i', args.files / 'INITRD.IMG', '::/' + shortcut])
    assert actual == expected, 'Kernel Console shortcut differs from the standard shortcut'
    assert manifest['initrd']['shortcuts'][shortcut] == hashlib.sha256(actual).hexdigest()


if __name__ == '__main__':
    main()
