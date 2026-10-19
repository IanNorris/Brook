#!/usr/bin/env python3
"""Rootless pathname stream-socket namespace/lifetime checks on a Unix FS."""
import argparse
from contextlib import ExitStack
import errno
import json
import os
from pathlib import Path
import socket
import stat
import tempfile


def main(parent):
    rows = []
    previous = os.getcwd()
    mask = os.umask(0o077)
    error = None
    def check(name, passed, **details):
        rows.append(dict(name=name, passed=bool(passed), **details))
        assert passed, name
    def fails(name, expected, action):
        actual = 0
        try:
            action()
        except OSError as exc:
            actual = exc.errno
        check(name, actual == expected, expected=expected, actual=actual)
    try:
        assert os.geteuid() != 0, 'Run as an ordinary user'
        with tempfile.TemporaryDirectory(prefix='unix-life-', dir=parent) as directory, ExitStack() as stack:
            root = Path(directory).resolve()
            def endpoint():
                s = stack.enter_context(socket.socket(socket.AF_UNIX, socket.SOCK_STREAM))
                s.settimeout(2)
                return s
            def exchange(listener, name, payload):
                client = endpoint()
                client.connect(str(name))
                accepted = stack.enter_context(listener.accept()[0])
                accepted.settimeout(2)
                client.sendall(payload)
                received = b''
                while len(received) < len(payload):
                    part = accepted.recv(len(payload) - len(received))
                    if not part:
                        break
                    received += part
                return received == payload

            path = root / 'first'
            first = endpoint()
            first.bind(str(path))
            info = path.lstat()
            check('bind_metadata', stat.S_ISSOCK(info.st_mode) and
                  info.st_uid == os.geteuid() and info.st_gid == os.getegid() and
                  stat.S_IMODE(info.st_mode) == 0o700 and info.st_nlink == 1)
            fails('duplicate_before_listen', errno.EADDRINUSE, lambda: endpoint().bind(str(path)))
            regular = root / 'file'
            regular.write_text('preserve')
            fails('bind_over_file', errno.EADDRINUSE, lambda: endpoint().bind(str(regular)))
            dangling = root / 'dangling'
            dangling.symlink_to('absent')
            fails('bind_over_dangling_link', errno.EADDRINUSE, lambda: endpoint().bind(str(dangling)))
            fails('missing_parent', errno.ENOENT, lambda: endpoint().bind(str(root / 'absent/socket')))
            locked = root / 'locked'
            locked.mkdir()
            locked.chmod(0o555)
            try:
                fails('parent_write_denied', errno.EACCES, lambda: endpoint().bind(str(locked / 'socket')))
            finally:
                locked.chmod(0o700)

            first.listen(4)
            os.chdir(root)
            check('relative_connect', exchange(first, 'first', b'relative'))
            alias = root / 'alias'
            alias.symlink_to('first')
            check('symlink_connect', exchange(first, alias, b'alias'))
            first_name = first.getsockname()
            check('getsockname_preserves_bind', first_name == str(path))
            path.chmod(0o400)
            try:
                fails('socket_write_denied', errno.EACCES, lambda: endpoint().connect(str(path)))
            finally:
                path.chmod(0o700)
            relative = endpoint()
            relative.bind('relative')
            relative.listen(1)
            check('relative_bind_absolute_connect', exchange(relative, root / 'relative', b'absolute'))

            old_client = endpoint()
            old_client.connect(str(path))
            old_peer = stack.enter_context(first.accept()[0])
            old_peer.settimeout(2)
            old_identity = (info.st_dev, info.st_ino)
            path.unlink()
            fails('unlinked_name_absent', errno.ENOENT, lambda: endpoint().connect(str(path)))
            replacement = endpoint()
            replacement.bind(str(path))
            new_info = path.lstat()
            new_identity = (new_info.st_dev, new_info.st_ino)
            check('unlinked_listener_pins_inode', new_identity != old_identity)
            replacement.listen(4)
            check('replacement_routes_new_listener', exchange(replacement, path, b'replacement'))
            first.close()
            old_client.sendall(b'old')
            check('accepted_stream_survives_unlink_and_listener_close', old_peer.recv(3) == b'old')
            after = path.lstat()
            check('old_close_preserves_replacement_node', (after.st_dev, after.st_ino) == new_identity)
            check('old_close_preserves_replacement_route', exchange(replacement, path, b'still new'))

            moved = root / 'moved'
            path.rename(moved)
            fails('renamed_old_name_absent', errno.ENOENT, lambda: endpoint().connect(str(path)))
            check('renamed_socket_routes_by_inode', exchange(replacement, moved, b'moved'))
            replacement.close()
            check('close_preserves_socket_node', stat.S_ISSOCK(moved.lstat().st_mode))
            fails('closed_listener_refused', errno.ECONNREFUSED, lambda: endpoint().connect(str(moved)))
            fails('closed_node_reserves_name', errno.EADDRINUSE, lambda: endpoint().bind(str(moved)))
            moved.unlink()
            rebound = endpoint()
            rebound.bind(str(moved))
            rebound.listen(1)
            check('explicit_unlink_allows_rebind', exchange(rebound, moved, b'rebound'))
            check('regular_file_preserved', regular.read_text() == 'preserve')
            os.chdir(previous)
    except Exception as exc:
        error = repr(exc)
    finally:
        os.chdir(previous)
        os.umask(mask)
    passed = error is None and all(row['passed'] for row in rows)
    print(json.dumps(dict(pass_all=passed, cases=rows, error=error)), flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--parent', default='/tmp')
    raise SystemExit(main(parser.parse_args().parent))
