#!/usr/bin/env python3
"""Check filesystem metadata and relative lookup for a pathname Unix socket.

Run as an ordinary user with --parent on a writable Unix filesystem.
An exit of one records missing semantics; it is not a passing Wine test.
"""
import argparse
import errno
import json
import os
from pathlib import Path
import socket
import stat
import tempfile


def main(parent):
    rows = []
    def check(name, passed, **details):
        rows.append(dict(name=name, passed=bool(passed), **details))
    with tempfile.TemporaryDirectory(prefix='unix-path-', dir=parent) as directory:
        path = str(Path(directory).resolve() / 'socket')
        previous_cwd = os.getcwd()
        previous_mask = os.umask(0o077)
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                listener.bind(path)
                listener.listen(1)
                try:
                    info = os.lstat(path)
                    check('bound_metadata', stat.S_ISSOCK(info.st_mode) and
                          info.st_uid == os.geteuid() and stat.S_IMODE(info.st_mode) == 0o700,
                          uid=info.st_uid, mode=oct(info.st_mode))
                except OSError as error:
                    check('bound_metadata', False, errno=error.errno)
                # An absolute bind and relative connect must name the same node.
                os.chdir(directory)
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                    try:
                        client.connect('socket')
                    except OSError as error:
                        check('relative_connect', False, errno=error.errno)
                    else:
                        with listener.accept()[0] as accepted:
                            client.sendall(b'Brook')
                            check('relative_connect', accepted.recv(5) == b'Brook')
                os.chdir(previous_cwd)
            try:
                info = os.lstat(path)
                check('node_survives_close', stat.S_ISSOCK(info.st_mode))
                os.unlink(path)
            except OSError as error:
                check('node_survives_close', False, errno=error.errno)
        finally:
            os.chdir(previous_cwd)
            os.umask(previous_mask)
    result = dict(pass_all=all(row['passed'] for row in rows), cases=rows)
    print(json.dumps(result), flush=True)
    return 0 if result['pass_all'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--parent', default='/tmp')
    raise SystemExit(main(parser.parse_args().parent))
