#!/usr/bin/env python3
"""Linux-matched exact-address mmap and non-destructive collision checks."""
import ctypes
import errno
import json
import mmap


def main():
    libc = ctypes.CDLL(None, use_errno=True)
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                          ctypes.c_int, ctypes.c_int, ctypes.c_long]
    libc.mmap.restype = ctypes.c_void_p
    libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    page = mmap.PAGESIZE
    failed = ctypes.c_void_p(-1).value
    base_flags = 0x22  # MAP_PRIVATE | MAP_ANONYMOUS
    noreplace = 0x100000
    checks = []
    owned = []

    def create(address, length, protection=3, extra=0):
        ctypes.set_errno(0)
        result = libc.mmap(address, length, protection,
                           base_flags | noreplace | extra, -1, 0)
        return result, ctypes.get_errno()

    def check(name, condition):
        assert condition, name
        checks.append(name)

    try:
        # Wine probes above Brook's ordinary automatic-mapping arena.
        # Never use plain MAP_FIXED to acquire an unowned address.
        address = 0x7F8800000000
        result, error = create(address, 3 * page)
        if result != failed:
            owned.append((result, 3 * page))
        check('exact-high-address', result == address)
        memory = (ctypes.c_ubyte * (3 * page)).from_address(address)
        memory[0], memory[page], memory[-1] = 17, 29, 43
        for name, start, length, extra in (
            ('same-range', address, 3 * page, 0),
            ('interior', address + page, page, 0),
            ('overlap-start', address - page, 2 * page, 0),
            ('overlap-end', address + 2 * page, 2 * page, 0),
            ('enclosing', address - page, 5 * page, 0),
            ('fixed-also-cannot-replace', address, page, 0x10),
        ):
            result, error = create(start, length, extra=extra)
            if result != failed and result != address:
                owned.append((result, length))
            check(name, result == failed and error == errno.EEXIST)
            check(name + '-bytes-preserved',
                  (memory[0], memory[page], memory[-1]) == (17, 29, 43))
        result, error = create(address + 1, page)
        check('unaligned-invalid', result == failed and error == errno.EINVAL)
        check('release-middle', libc.munmap(address + page, page) == 0)
        result, error = create(address + page, page, protection=0)
        check('reuse-hole-exactly', result == address + page)
        result, error = create(address + page, page)
        check('prot-none-is-occupied', result == failed and error == errno.EEXIST)
        result, error = create(address + 3 * page, 1)
        if result != failed:
            owned.append((result, page))
        check('adjacent-and-rounded-length', result == address + 3 * page)
        result, error = create(address + 3 * page, page)
        check('rounded-tail-is-occupied', result == failed and error == errno.EEXIST)
        print(json.dumps(dict(passed=True, checks=checks)))
        return 0
    except Exception as error:
        print(json.dumps(dict(passed=False, checks=checks, error=str(error))))
        return 1
    finally:
        for address, length in reversed(owned):
            libc.munmap(address, length)


if __name__ == '__main__':
    raise SystemExit(main())
