#!/usr/bin/env python3
"""Anonymous mmap semantics and physical cost; Linux control or Brook guest.

Usage: python3 anonymous_memory.py RESULT.json [--expect-lazy]
Physical accounting assertions require an otherwise idle Brook VM. Linux
controls check semantics and report physical samples without a noise-sensitive
host-memory assertion. All mappings and pipe descriptors are private fixtures.
"""
import ctypes
import json
import os
from pathlib import Path
import sys
import time

libc = ctypes.CDLL(None, use_errno=True)
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                      ctypes.c_int, ctypes.c_int, ctypes.c_long]
libc.mmap.restype = ctypes.c_void_p
libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
libc.madvise.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
libc.read.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t]
libc.read.restype = ctypes.c_ssize_t
PAGE = 4096
SIZE = 256 * 1024 * 1024
strict = '--expect-lazy' in sys.argv[2:]
results = {}


def free_bytes():
    for line in Path('/proc/meminfo').read_text().splitlines():
        if line.startswith('MemFree:'):
            return int(line.split()[1]) * 1024
    raise AssertionError('MemFree missing')


def mapping(size, flags=0x22):
    p = libc.mmap(None, size, 3, flags, -1, 0)
    assert p is not None and p != ctypes.c_void_p(-1).value, ctypes.get_errno()
    return p


def case(name, fn):
    try:
        results[name] = dict(passed=True, detail=fn())
    except BaseException as exc:
        results[name] = dict(passed=False, error=repr(exc))
    print('ANON_MEMORY_CASE', name, json.dumps(results[name]), flush=True)


before = free_bytes()
base = mapping(SIZE)
after_reserve = free_bytes()


def reservation():
    consumed = before - after_reserve
    if strict:
        assert consumed < 16 * 1024 * 1024, consumed
    return dict(reserved=SIZE, physical_delta=consumed, enforced=strict)


def sparse_touch():
    for off in (0, PAGE, SIZE // 2, SIZE - PAGE):
        assert ctypes.string_at(base + off, PAGE) == bytes(PAGE)
        ctypes.c_ubyte.from_address(base + off).value = 0x42
    delta = before - free_bytes()
    if strict:
        assert delta < 16 * 1024 * 1024, delta
    return dict(touched_pages=4, physical_delta=delta)


def kernel_copyout():
    rd, wr = os.pipe()
    try:
        os.write(wr, b'copyout')
        assert libc.read(rd, base + 8 * PAGE, 7) == 7, ctypes.get_errno()
        assert ctypes.string_at(base + 8 * PAGE, 7) == b'copyout'
    finally:
        os.close(rd)
        os.close(wr)
    return 'read syscall faults in an untouched destination page'


def fork_cow():
    child = os.fork()
    if not child:
        try:
            ctypes.c_ubyte.from_address(base).value = 0x55
            ctypes.c_ubyte.from_address(base + 10 * PAGE).value = 0x66
            os._exit(0)
        except BaseException:
            os._exit(1)
    waited, status = os.waitpid(child, 0)
    assert waited == child and status == 0, status
    assert ctypes.c_ubyte.from_address(base).value == 0x42
    assert ctypes.c_ubyte.from_address(base + 10 * PAGE).value == 0
    return 'touched COW and previously absent child page stay private'


def protect():
    assert libc.mprotect(base, 2 * PAGE, 0) == 0, ctypes.get_errno()
    assert libc.mprotect(base, 2 * PAGE, 3) == 0, ctypes.get_errno()
    assert ctypes.c_ubyte.from_address(base).value == 0x42
    # Change permissions on a still-absent page, then fault it in.
    assert libc.mprotect(base + 12 * PAGE, PAGE, 1) == 0, ctypes.get_errno()
    assert ctypes.string_at(base + 12 * PAGE, PAGE) == bytes(PAGE)
    assert libc.mprotect(base + 12 * PAGE, PAGE, 3) == 0, ctypes.get_errno()
    ctypes.c_ubyte.from_address(base + 12 * PAGE).value = 0x77
    return 'PROT_NONE restores contents; absent page follows updated protection'


def discard():
    assert libc.madvise(base, PAGE, 4) == 0, ctypes.get_errno()
    assert ctypes.string_at(base, PAGE) == bytes(PAGE)
    assert ctypes.c_ubyte.from_address(base + PAGE).value == 0x42
    return 'MADV_DONTNEED zeros only the discarded page'


def populate():
    size = 16 * 1024 * 1024
    start = free_bytes()
    p = mapping(size, 0x22 | 0x8000)
    try:
        consumed = start - free_bytes()
        if strict:
            assert consumed >= size - 1024 * 1024, consumed
        assert ctypes.string_at(p + size - PAGE, PAGE) == bytes(PAGE)
        return dict(reserved=size, physical_delta=consumed, enforced=strict)
    finally:
        assert libc.munmap(p, size) == 0


def shared():
    p = mapping(PAGE, 0x21)
    try:
        child = os.fork()
        if not child:
            ctypes.c_ubyte.from_address(p).value = 0x39
            os._exit(0)
        assert os.waitpid(child, 0)[1] == 0
        assert ctypes.c_ubyte.from_address(p).value == 0x39
        return 'shared anonymous child write remains visible to parent'
    finally:
        assert libc.munmap(p, PAGE) == 0


try:
    for name, fn in [('reservation', reservation), ('sparse_touch', sparse_touch),
                     ('kernel_copyout', kernel_copyout), ('fork_cow', fork_cow),
                     ('protect', protect), ('discard', discard),
                     ('populate', populate), ('shared', shared)]:
        case(name, fn)
finally:
    assert libc.munmap(base, SIZE) == 0
    time.sleep(0.3)

results['final_memory'] = dict(before=before, after=free_bytes())
Path(sys.argv[1]).write_text(json.dumps(results, indent=2) + '\n')
passed = sum(v.get('passed', False) for v in results.values())
print('ANON_MEMORY_RESULT', passed, '/8', flush=True)
sys.exit(0 if passed == 8 else 1)
