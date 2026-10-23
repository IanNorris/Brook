#!/usr/bin/env python3
"""VM-only x86-64 shared-memfd futex regression, including different-VA rights."""
import array
import argparse
import ctypes as c
import errno
import json
import mmap
import os
import signal
import socket
import struct
import time
import threading
import tempfile

libc = c.CDLL(None, use_errno=True)
call = libc.syscall
call.restype = c.c_long
call.argtypes = [c.c_long, c.c_void_p, c.c_long, c.c_long, c.c_void_p, c.c_void_p, c.c_ulong]
class Timespec(c.Structure):
    _fields_ = [('sec', c.c_long), ('ns', c.c_long)]


def address(view, offset=0):
    return c.addressof(c.c_uint32.from_buffer(view, offset))


def futex(addr, op, value, timeout=None, mask=0xffffffff):
    c.set_errno(0)
    result = call(202, addr, op, value, c.byref(timeout) if timeout else None, None, mask)
    return result, c.get_errno()


def new_file(directory, name):
    if directory is None:
        return os.memfd_create(name, os.MFD_CLOEXEC)
    fd,path=tempfile.mkstemp(prefix=name+'-',dir=directory)
    os.unlink(path)
    return fd


def remap_race(private, directory=None):
    """Stress admission against replacement mappings; not a deterministic hook."""
    map_page = libc.mmap
    map_page.argtypes = [c.c_void_p, c.c_size_t, c.c_int, c.c_int, c.c_int, c.c_long]
    map_page.restype = c.c_void_p
    unmap = libc.munmap
    unmap.argtypes = [c.c_void_p, c.c_size_t]
    unmap.restype = c.c_int
    descriptors = [new_file(directory, 'futex-remap') for _ in range(2)]
    for fd in descriptors:
        os.ftruncate(fd, 4096)
        os.pwrite(fd, struct.pack('I', 7), 0)
    base = map_page(None, 4096, 3, mmap.MAP_SHARED, descriptors[0], 0)
    assert base not in (None, c.c_void_p(-1).value)
    stop = threading.Event()
    errors = []
    replacements = [0]
    def churn():
        try:
            while not stop.is_set():
                # MAP_FIXED replaces the same VA with a different object.
                fd = descriptors[replacements[0] & 1]
                assert map_page(base, 4096, 3, mmap.MAP_SHARED | 0x10, fd, 0) == base
                replacements[0] += 1
        except BaseException as error:
            errors.append(repr(error))
    worker = threading.Thread(target=churn)
    worker.start()
    try:
        for _ in range(256):
            result, error = futex(base, 128 if private else 0, 7, Timespec(0, 1000000))
            assert result == -1 and error in (errno.EAGAIN, errno.EFAULT, errno.ETIMEDOUT, errno.EINTR), (result, error)
    finally:
        stop.set(); worker.join(10)
        assert not worker.is_alive(), 'remap thread did not stop'
        assert unmap(base, 4096) == 0
        for fd in descriptors:
            os.close(fd)
    assert not errors and replacements[0] > 0, (errors, replacements)
    return replacements[0]


def main(directory=None):
    cases = []
    def run(name, offset=0, private=False, bitset=False, anonymous=False, interrupt=False, cancel=False):
        fd = -1 if anonymous else new_file(directory, 'brook-futex-check')
        if fd >= 0:
            os.ftruncate(fd, 8192)
        view = mmap.mmap(fd, 8192, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ | mmap.PROT_WRITE)
        view[0:4] = struct.pack('I', 7); view[4096:4100] = struct.pack('I', 7)
        other = new_file(directory, 'brook-futex-other'); os.ftruncate(other, 8192)
        unrelated = mmap.mmap(other, 8192, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ | mmap.PROT_WRITE)
        a, b = socket.socketpair()
        pid = os.fork()
        if pid == 0:
            try:
                a.close(); unrelated.close(); os.close(other)
                if anonymous:
                    alias = view
                else:
                    os.close(fd)
                    _, controls, _, _ = b.recvmsg(1, socket.CMSG_SPACE(4))
                    received = array.array('i', controls[0][2])[0]
                    alias = mmap.mmap(received, 8192, flags=mmap.MAP_SHARED,
                                      prot=mmap.PROT_READ | mmap.PROT_WRITE)
                    os.close(received)
                    assert address(alias, offset) != address(view, offset)
                    view.close()
                if interrupt:
                    signal.signal(signal.SIGUSR1, lambda *_: None)
                b.sendall(struct.pack('Q', address(alias, offset)))
                timeout = Timespec(0, 200000000) if private else Timespec(5, 0)
                if bitset:
                    # WAIT_BITSET uses an absolute CLOCK_MONOTONIC deadline.
                    end = time.monotonic_ns() + 5000000000
                    timeout = Timespec(end // 1000000000, end % 1000000000)
                result, error = futex(address(alias, offset), (9 if bitset else 0) | (128 if private else 0),
                                      7, timeout, 1)
                if private:
                    assert result == -1 and error == errno.ETIMEDOUT, (result, error)
                elif interrupt:
                    assert result == -1 and error == errno.EINTR, (result, error)
                else:
                    assert result == 0, (result, error)
                alias.close(); b.close()
            except BaseException:
                os._exit(1)
            os._exit(0)
        b.close()
        if not anonymous:
            a.sendmsg([b'F'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [fd]))])
            os.close(fd)
        raw = a.recv(8); assert len(raw) == 8
        child_address = struct.unpack('Q', raw)[0]
        assert anonymous or child_address != address(view, offset)
        # Distinct objects, offsets, private domains and bitsets must not match.
        assert futex(address(unrelated, offset), 1, 1)[0] == 0
        assert futex(address(view, 4096 if offset == 0 else 0), 1, 1)[0] == 0
        assert futex(address(view, offset), 129, 1)[0] == 0
        if bitset:
            assert futex(address(view, offset), 10, 1, mask=2)[0] == 0
        if cancel:
            os.kill(pid, signal.SIGKILL)
            assert os.waitpid(pid, 0) == (pid, signal.SIGKILL)
            # Cleanup may be deferred; safely consuming an old waiter is legal.
            assert futex(address(view, offset), 1, 1)[0] in (0, 1)
            assert futex(address(view, offset), 1, 1)[0] == 0
        elif private:
            assert futex(address(view, offset), 1, 1)[0] == 0
            assert os.waitpid(pid, 0) == (pid, 0)
        elif interrupt:
            end = time.monotonic() + 5
            while True:
                finished, status = os.waitpid(pid, os.WNOHANG)
                if finished:
                    assert status == 0, status
                    break
                assert time.monotonic() < end
                os.kill(pid, signal.SIGUSR1); time.sleep(.01)
            assert futex(address(view, offset), 1, 1)[0] == 0
        else:
            end = time.monotonic() + 5
            while True:
                count, error = futex(address(view, offset), 10 if bitset else 1, 1, mask=1)
                assert count >= 0, error
                if count:
                    assert count == 1
                    break
                assert time.monotonic() < end, name
                time.sleep(.001)
            assert os.waitpid(pid, 0) == (pid, 0)
        a.close(); view.close(); unrelated.close(); os.close(other)
        cases.append(name)
    for i in range(12):
        run('rights-alias-' + str(i), offset=4096 if i & 1 else 0)
    run('bitset-alias', bitset=True)
    run('private-isolation', private=True)
    run('fork-anonymous-shared', anonymous=True)
    run('signal-cleanup', interrupt=True)
    run('fatal-cancellation', cancel=True)
    remaps = {}
    for private in (False, True):
        label = 'private-remap-race' if private else 'shared-remap-race'
        remaps[label] = remap_race(private, directory)
        cases.append(label)
    print(json.dumps(dict(passed=True, cases=cases, remaps=remaps,
                         backing='file' if directory else 'memfd',
                         scope='shared object identity/offset, private isolation, cleanup and remap stress')), flush=True)


if __name__ == '__main__':
    signal.alarm(90)
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory',help='Use ordinary unlinked files on this filesystem instead of memfd')
    main(parser.parse_args().directory)
