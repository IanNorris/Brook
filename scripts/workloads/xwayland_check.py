#!/usr/bin/env python3
"""Run inside Brook: start a rootful diagnostic Xwayland on the guest desktop.

The existing hard-link lockfile limitation requires -nolock. This helper does
not select or connect to a host display. It is not the normal desktop launcher.
"""
import argparse
import os
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--display', type=int, default=0)
    parser.add_argument('--glamor', choices=['gl', 'es', 'off'], default='gl')
    parser.add_argument('--software', action='store_true', help='Request Mesa software rendering')
    args = parser.parse_args()
    if not 0 <= args.display <= 63:
        parser.error('display must be between 0 and 63')
    program = Path('/nix/profile/bin/Xwayland').resolve(strict=True)
    os.environ.update(HOME='/data/home', XDG_RUNTIME_DIR='/tmp', WAYLAND_DISPLAY='wayland-0')
    command = ['Xwayland', f':{args.display}', '-ac', '-nolisten', 'tcp', '-nolock',
               '-noreset', '-geometry', '800x600']
    if args.glamor == 'off':
        os.environ['XWAYLAND_NO_GLAMOR'] = '1'
        command.append('-shm')
    else:
        os.environ.pop('XWAYLAND_NO_GLAMOR', None)
        command += ['-glamor', args.glamor]
    if args.software:
        os.environ['LIBGL_ALWAYS_SOFTWARE'] = '1'
    os.execv(str(program), command)


if __name__ == '__main__':
    main()
