#!/usr/bin/env python3
"""Linux/guest shared-file mmap coherence, permissions, fork and EOF checks."""
import ctypes
import errno
import json
import mmap
import os
from pathlib import Path
import resource
import signal
import tempfile


def main():
    rows = []
    libc = ctypes.CDLL(None, use_errno=True)
    libc.mmap.restype = ctypes.c_void_p
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                         ctypes.c_int, ctypes.c_int, ctypes.c_long]
    libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]

    def check(name, passed):
        rows.append({'name': name, 'passed': bool(passed)})
        assert passed, name

    try:
        with tempfile.TemporaryDirectory(prefix='shared-file-', dir='/tmp') as directory:
            path = Path(directory) / 'backing'
            fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
            os.ftruncate(fd, 8192 + 37)
            os.pwrite(fd, b'initial', 0)
            second_fd = os.open(path, os.O_RDWR)
            read_fd = os.open(path, os.O_RDONLY)
            try:
                with mmap.mmap(fd, 8192, flags=mmap.MAP_SHARED,
                               prot=mmap.PROT_READ | mmap.PROT_WRITE) as first, \
                     mmap.mmap(second_fd, 8192, flags=mmap.MAP_SHARED,
                               prot=mmap.PROT_READ | mmap.PROT_WRITE) as second, \
                     mmap.mmap(fd, 8192, flags=mmap.MAP_PRIVATE,
                               prot=mmap.PROT_READ | mmap.PROT_WRITE) as private:
                    check('independent_open_initial_content', second[:7] == b'initial')
                    private[0:7] = b'PRIVATE'
                    check('private_cow_isolation', first[:7] == b'initial' and private[:7] == b'PRIVATE')
                    first[0:6] = b'SHARED'
                    check('shared_store_visible_to_second_open', second[:6] == b'SHARED')
                    check('mapped_store_visible_to_pread', os.pread(read_fd, 6, 0) == b'SHARED')
                    os.pwrite(second_fd, b'PWRITE', 4096)
                    check('pwrite_visible_to_both_mappings', first[4096:4102] == second[4096:4102] == b'PWRITE')
                    check('private_copy_preserved', private[:7] == b'PRIVATE')
                    first.flush(0, 4096)
                    second.flush(4096, 4096)
                    check('range_msync', os.pread(read_fd, 6, 0) == b'SHARED')

                    child = os.fork()
                    if child == 0:
                        second[128:133] = b'CHILD'
                        second.flush(0, 4096)
                        os._exit(0)
                    _, status = os.waitpid(child, 0)
                    check('fork_shared_write', status == 0 and first[128:133] == b'CHILD')
                    check('fork_private_unchanged', private[:7] == b'PRIVATE')

                    # A readonly descriptor may never acquire shared write
                    # permission, including a later mprotect upgrade.
                    raw = libc.mmap(None, 4096, mmap.PROT_READ, mmap.MAP_SHARED, read_fd, 0)
                    assert raw not in (None, ctypes.c_void_p(-1).value)
                    try:
                        ctypes.set_errno(0)
                        result = libc.mprotect(raw, 4096, mmap.PROT_READ | mmap.PROT_WRITE)
                        check('readonly_descriptor_upgrade_denied', result == -1 and ctypes.get_errno() == errno.EACCES)
                    finally:
                        assert libc.munmap(raw, 4096) == 0

                    raw = libc.mmap(None, 16384, mmap.PROT_READ | mmap.PROT_WRITE,
                                    mmap.MAP_SHARED, second_fd, 0)
                    assert raw not in (None, ctypes.c_void_p(-1).value)
                    try:
                        child = os.fork()
                        if child == 0:
                            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
                            ctypes.string_at(raw + 12288, 1)
                            os._exit(99)
                        _, status = os.waitpid(child, 0)
                        check('past_eof_fault_is_sigbus', os.WIFSIGNALED(status) and os.WTERMSIG(status) == signal.SIGBUS)
                    finally:
                        assert libc.munmap(raw, 16384) == 0

                    os.close(fd); fd = -1
                    os.close(second_fd); second_fd = -1
                    second[256:261] = b'CLOSE'
                    first.flush()
                    check('mappings_survive_all_writable_fd_closes', first[256:261] == b'CLOSE' and os.pread(read_fd, 5, 256) == b'CLOSE')
            finally:
                for descriptor in (fd, second_fd, read_fd):
                    if descriptor >= 0:
                        os.close(descriptor)

            tail = Path(directory) / 'tail'
            fd = os.open(tail, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
            try:
                os.write(fd, b'x' * 17)
                raw = libc.mmap(None, 4096, mmap.PROT_READ | mmap.PROT_WRITE, mmap.MAP_SHARED, fd, 0)
                assert raw not in (None, ctypes.c_void_p(-1).value)
                try:
                    ctypes.c_ubyte.from_address(raw + 80).value = 0xAB
                    os.pwrite(fd, b'Z', 128)
                    check('extending_write_zeroes_old_eof_gap', os.pread(fd, 112, 17) == b'\0' * 111 + b'Z')
                    # The mapping must agree with the ordinary read path.
                    check('mapped_gap_agrees_with_file', ctypes.string_at(raw + 17, 112) == b'\0' * 111 + b'Z')
                finally:
                    assert libc.munmap(raw, 4096) == 0
            finally:
                os.close(fd)
    except Exception as error:
        print(json.dumps({'pass_all': False, 'cases': rows, 'error': repr(error),
                          'errno': getattr(error, 'errno', None)}))
        return 1
    print(json.dumps({'pass_all': True, 'cases': rows}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
