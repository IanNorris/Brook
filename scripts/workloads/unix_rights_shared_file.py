#!/usr/bin/env python3
"""Exercise Wine's unlinked-file/SCM_RIGHTS/shared-mmap IPC pattern."""
import array
import json
import mmap
import os
import socket
import tempfile


def main():
    cases = []
    def check(name, value):
        cases.append({'name': name, 'passed': bool(value)})
        assert value, name
    try:
        with tempfile.TemporaryFile(dir='/tmp') as backing:
            backing.truncate(4096)
            check('backing_unlinked', os.fstat(backing.fileno()).st_nlink == 0)
            a, b = socket.socketpair()
            try:
                a.sendmsg([b'x'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                   array.array('i', [backing.fileno()]))])
                data, ancillary, flags, _ = b.recvmsg(1, socket.CMSG_SPACE(4))
                check('rights_payload', data == b'x' and not flags & socket.MSG_CTRUNC)
                fds = array.array('i')
                for level, kind, raw in ancillary:
                    if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                        fds.frombytes(raw)
                assert len(fds) == 1
                received = fds[0]
                try:
                    original = os.fstat(backing.fileno())
                    transferred = os.fstat(received)
                    check('rights_inode_identity', (original.st_dev, original.st_ino) ==
                          (transferred.st_dev, transferred.st_ino))
                    with mmap.mmap(received, 4096, flags=mmap.MAP_SHARED,
                                   prot=mmap.PROT_READ | mmap.PROT_WRITE) as view:
                        view[:5] = b'BROOK'
                        check('mapped_store_visible_to_pread', os.pread(backing.fileno(), 5, 0) == b'BROOK')
                        os.pwrite(backing.fileno(), b'WINE', 64)
                        check('pwrite_visible_to_mapping', view[64:68] == b'WINE')
                        view.flush()
                        check('msync_preserves_data', os.pread(received, 5, 0) == b'BROOK')
                finally:
                    os.close(received)
            finally:
                a.close(); b.close()
    except Exception as error:
        print(json.dumps({'pass_all': False, 'cases': cases, 'error': repr(error),
                          'errno': getattr(error, 'errno', None)}))
        return 1
    print(json.dumps({'pass_all': True, 'cases': cases}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
