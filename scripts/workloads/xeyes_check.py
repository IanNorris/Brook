#!/usr/bin/env python3
"""Run stock xeyes against a guest Xwayland display; never use host DISPLAY."""
import argparse
import os
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--display', type=int, default=0)
    parser.add_argument('--geometry', default='320x200+40+40')
    parser.add_argument('--no-shape', action='store_true')
    parser.add_argument('--classic', action='store_true', help='Use core X11 drawing instead of XRender')
    args = parser.parse_args()
    if not 0 <= args.display <= 63:
        parser.error('display must be between 0 and 63')
    if not re.fullmatch(r'[1-9][0-9]{0,3}x[1-9][0-9]{0,3}[+-][0-9]{1,4}[+-][0-9]{1,4}', args.geometry):
        parser.error('geometry must be WIDTHxHEIGHT+X+Y (negative offsets are also allowed)')
    program = Path('/nix/profile/bin/xeyes').resolve(strict=True)
    os.environ['DISPLAY'] = f':{args.display}'
    command = ['xeyes', '-display', os.environ['DISPLAY'], '-geometry', args.geometry]
    if args.no_shape:
        command.append('+shape')
    if args.classic:
        command.append('+render')
    os.execv(str(program), command)


if __name__ == '__main__':
    main()
