#!/usr/bin/env python3
"""Brook-only native window geometry ABI control; briefly creates one window.

Run on an unattended guest after installing a kernel with syscall 0xB08.
Does not use WM_GET_WINDOWS, inspect foreign windows, or map their pixels.
"""
import argparse
import ctypes as c
import errno
import json
import os
import platform


class Create(c.Structure):
    _fields_ = [('window', c.c_uint32), ('stride', c.c_uint32), ('pixels', c.c_uint64)]


class State(c.Structure):
    _fields_ = [('version', c.c_uint32), ('window', c.c_uint32),
                ('incarnation', c.c_uint64), ('x', c.c_int32), ('y', c.c_int32),
                ('width', c.c_uint32), ('height', c.c_uint32),
                ('flags', c.c_uint32), ('reserved', c.c_uint32)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--brook-guest', action='store_true', required=True)
    parser.parse_args()
    if platform.machine() != 'x86_64' or c.sizeof(State) != 40:
        raise RuntimeError('requires x86-64 window-state ABI')
    libc = c.CDLL(None, use_errno=True)
    libc.syscall.restype = c.c_long

    def call(number, *args):
        c.set_errno(0)
        result = libc.syscall(c.c_long(number), *(c.c_uint64(a) for a in args))
        return -c.get_errno() if result == -1 else result

    checks = {}
    created = Create()
    title = c.create_string_buffer(b'Window geometry control')
    result = call(506, 320, 200, c.addressof(title), c.addressof(created), 1, 0)
    if result or not created.window:
        raise RuntimeError(f'window creation failed: {result}')
    try:
        state = State()
        pointer = c.addressof(state)
        size = c.sizeof(state)
        query = lambda incarnation=0: call(0xB08, created.window, incarnation, pointer, size, 0, 0)
        checks['query'] = query() == 0
        checks['version_identity'] = state.version == 1 and state.window == created.window and bool(state.incarnation)
        checks['client_extent'] = (state.width, state.height) == (320, 200)
        checks['client_chrome'] = bool(state.flags & 16)
        checks['reserved_zero'] = state.reserved == 0
        checks['exact_incarnation'] = query(state.incarnation) == 0
        identity = state.incarnation
        checks['place_window'] = call(0xB09, created.window, identity, 100, 120, 0, 0) == 0
        checks['placed_client_origin'] = query(identity) == 0 and (state.x, state.y) == (100, 120)
        checks['place_preserves_size'] = (state.width, state.height) == (320, 200)
        checks['place_stale'] = call(0xB09, created.window, identity ^ (1 << 63), 1, 1, 0, 0) == -errno.ESTALE
        checks['place_requires_identity'] = call(0xB09, created.window, 0, 1, 1, 0, 0) == -errno.EINVAL
        checks['place_bad_coordinate'] = call(0xB09, created.window, identity, 1 << 40, 1, 0, 0) == -errno.EINVAL
        before = bytes(state)
        checks['stale_incarnation'] = query(state.incarnation ^ (1 << 63)) == -errno.ESTALE
        checks['error_output_unchanged'] = bytes(state) == before
        checks['bad_size'] = call(0xB08, created.window, 0, pointer, size-1, 0, 0) == -errno.EINVAL
        checks['bad_flags'] = call(0xB08, created.window, 0, pointer, size, 1, 0) == -errno.EINVAL
        checks['bad_reserved'] = call(0xB08, created.window, 0, pointer, size, 0, 1) == -errno.EINVAL
        checks['bad_window'] = call(0xB08, 0, 0, pointer, size, 0, 0) == -errno.EINVAL
        checks['bad_pointer'] = call(0xB08, created.window, 0, 0, size, 0, 0) == -errno.EFAULT
        child = os.fork()
        if child == 0:
            os._exit(0 if query() == -errno.ENOENT else 1)
        _, status = os.waitpid(child, 0)
        checks['foreign_owner_hidden'] = os.waitstatus_to_exitcode(status) == 0
    finally:
        call(507, created.window, 0, 0, 0, 0, 0)
    checks['destroyed_window_hidden'] = query() == -errno.ENOENT
    # Provider extents must not be expanded to interactive resize minima.
    tiny = Create()
    result = call(506, 32, 24, c.addressof(title), c.addressof(tiny), 3, 0)
    checks['tiny_create'] = result == 0 and bool(tiny.window)
    if result == 0 and tiny.window:
        try:
            checks['tiny_exact_extent'] = call(0xB08, tiny.window, 0, pointer, size, 0, 0) == 0 and (state.width, state.height) == (32, 24)
        finally:
            call(507, tiny.window, 0, 0, 0, 0, 0)
    print(json.dumps(dict(window_state_pass=all(checks.values()), checks=checks)), flush=True)
    return int(not all(checks.values()))


if __name__ == '__main__':
    raise SystemExit(main())
