#!/usr/bin/env python3
"""Run XWM controls against a fresh headless X server, never the user's display."""
import argparse
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import tempfile

# A one-image Xcursor "left_ptr" with a size and hotspot that neither the X
# server's default "X" glyph nor the core cursor font arrow has. The fixture
# reads the root cursor back through XFixes.
CURSOR_SIZE, CURSOR_HOT = (9, 7), (3, 5)


def write_cursor_theme(root):
    cursors = Path(root) / 'icons/default/cursors'
    cursors.mkdir(parents=True)
    (width, height), (xhot, yhot) = CURSOR_SIZE, CURSOR_HOT
    image = struct.pack('<9I', 36, 0xfffd0002, 8, 1, width, height, xhot, yhot, 0)
    image += struct.pack('<I', 0xff00c000) * (width * height)
    header = b'Xcur' + struct.pack('<3I', 16, 0x10000, 1) + struct.pack('<3I', 0xfffd0002, 8, 28)
    (cursors / 'left_ptr').write_bytes(header + image)
    return str(Path(root) / 'icons')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xvfb', type=Path, required=True)
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, required=True)
    args = parser.parse_args()
    processes = []
    with tempfile.TemporaryDirectory(prefix='brook-xwm-control-') as tmp:
        with (Path(tmp) / 'server.log').open('wb') as log:
            read_fd, write_fd = os.pipe()
            try:
                server = subprocess.Popen([str(args.xvfb), '-displayfd', str(write_fd),
                                           '-nolisten', 'tcp', '-ac', '-screen', '0', '800x600x24'],
                                          pass_fds=(write_fd,), stdout=log, stderr=log)
                processes.append(server)
                os.close(write_fd)
                if not select.select([read_fd], [], [], 10)[0]:
                    raise RuntimeError('isolated X server did not become ready')
                display = os.read(read_fd, 32).strip()
                if not display.isdigit() or server.poll() is not None:
                    raise RuntimeError('isolated X server failed readiness')
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as wm, _control_pair() as control:
                    # Connecting directly avoids any fallback to ambient DISPLAY.
                    wm.connect('/tmp/.X11-unix/X' + display.decode())
                    theme = dict(os.environ, XCURSOR_PATH=write_cursor_theme(tmp), XCURSOR_THEME='default',
                                 XCURSOR_SIZE='8')
                    helper = subprocess.Popen([str(args.helper), str(wm.fileno()),
                                               str(control[0].fileno()), '7'],
                                              pass_fds=(wm.fileno(), control[0].fileno()), stdout=log, stderr=log,
                                              env=theme)
                    processes.append(helper)
                    control[0].close()
                    expect = '%dx%d+%d+%d' % (CURSOR_SIZE + CURSOR_HOT)
                    result = subprocess.run([str(args.fixture), ':' + display.decode(), str(control[1].fileno())],
                                            pass_fds=(control[1].fileno(),), timeout=30,
                                            env=dict(os.environ, BROOK_XWM_EXPECT_ROOT_CURSOR=expect))
                    if result.returncode:
                        raise RuntimeError('XWM fixture failed')
            finally:
                os.close(read_fd)
                for process in reversed(processes):
                    if process.poll() is None:
                        process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
    return 0


from contextlib import contextmanager


@contextmanager
def _control_pair():
    pair = socket.socketpair()
    try:
        yield pair
    finally:
        for stream in pair:
            stream.close()


if __name__ == '__main__':
    raise SystemExit(main())
