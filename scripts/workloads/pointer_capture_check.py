#!/usr/bin/env python3
"""Run in Brook; wait for the host's bounded PS/2 relative-input control.

This tests native window leases, not Wayland or physical host-pointer grabbing.
After raw-input-ready: send ten (200,-100) relative movements, left-down,
ten further movements, wait for consumption, then Ctrl+Alt+Esc while holding
left. Move to the launcher with left still held, release left. The controller
must select a relative QEMU mouse first. No host desktop input is used.
"""
import argparse
import ctypes
import errno
import json
import time


class Window(ctypes.Structure):
    _fields_ = [('id', ctypes.c_uint32), ('stride', ctypes.c_uint32),
                ('pixels', ctypes.c_uint64)]


class Event(ctypes.Structure):
    _fields_ = [('type', ctypes.c_uint8), ('scan', ctypes.c_uint8),
                ('ascii', ctypes.c_uint8), ('mods', ctypes.c_uint8),
                ('x', ctypes.c_int16), ('y', ctypes.c_int16),
                ('token', ctypes.c_uint32)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--timeout', type=float, default=90)
    args = parser.parse_args()
    if not 1 <= args.timeout <= 180:
        parser.error('timeout must be 1–180 seconds')
    libc = ctypes.CDLL(None, use_errno=True)
    libc.syscall.restype = ctypes.c_long

    def call(number, *values):
        packed = [ctypes.c_long(v if isinstance(v, int)
                              else ctypes.cast(v, ctypes.c_void_p).value) for v in values]
        result = libc.syscall(ctypes.c_long(number), *packed)
        return -ctypes.get_errno() if result == -1 else result

    windows = []

    def create(title):
        out = Window()
        name = ctypes.create_string_buffer(title.encode())
        assert call(506, 320, 240, name, ctypes.byref(out), 0, 0) == 0
        assert out.id and out.pixels and out.stride >= 320
        windows.append(out.id)
        ctypes.memset(out.pixels, 0x55, out.stride * 240 * 4)
        assert call(508, out.id) == 0
        return out.id

    def capture(window, operation=0, token=0):
        return call(0xB0A, window, operation, token)

    try:
        first = create('Pointer capture control')
        lease = capture(first, 1)
        assert lease > 0, lease
        events = (Event * 64)()
        assert ctypes.sizeof(Event) == 12
        call(510, first, events, 64)  # Initial focus notification.
        print(json.dumps(dict(phase='raw-input-ready', window=first, lease=lease)), flush=True)
        deadline = time.monotonic() + args.timeout
        dx = dy = 0
        downs = []
        released = False
        while time.monotonic() < deadline:
            count = call(510, first, events, 64)
            assert 0 <= count <= 64, count
            for event in events[:count]:
                if event.type == 6:
                    assert event.token == lease
                    dx += event.x
                    dy += event.y
                elif event.type == 3:
                    assert event.token == lease
                    downs.append((event.scan, dx, dy))
            current = capture(first, 0, lease)
            assert current in (0, lease), current
            if not current:
                released = True
                break
            time.sleep(.01)
        assert released, 'emergency release was not observed'
        assert (dx, dy) == (4000, -2000), (dx, dy)
        assert downs == [(0, 2000, -1000)], downs
        # Must not re-acquire through a still-held button. Once the host
        # releases it over the launcher, no accidental popup may block capture.
        second_lease = -errno.EAGAIN
        end = time.monotonic() + 5
        while second_lease == -errno.EAGAIN and time.monotonic() < end:
            second_lease = capture(first, 1)
            if second_lease == -errno.EAGAIN:
                time.sleep(.01)
        assert second_lease > lease, second_lease
        assert capture(first, 2, lease) == -errno.ESTALE
        assert capture(first, 0) == second_lease
        other = create('Focus revocation control')
        assert capture(first, 0) == 0
        assert capture(first, 1) == -errno.EAGAIN
        other_lease = capture(other, 1)
        assert other_lease > second_lease
        assert call(507, other) == 0
        assert capture(other, 0) == -errno.ENOENT
        final = capture(first, 1)
        assert final > other_lease
        assert capture(first, 2, second_lease) == -errno.ESTALE
        assert capture(first, 2, final) == 0
        print(json.dumps(dict(phase='complete', raw_delta=[dx, dy], down_positions=downs,
                              emergency_release=True, focus_revocation=True,
                              destruction_revocation=True, stale_lease_rejected=True)), flush=True)
    finally:
        for window in reversed(windows):
            call(507, window)


if __name__ == '__main__':
    main()
