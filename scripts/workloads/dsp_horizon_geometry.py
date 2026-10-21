#!/usr/bin/env python3
"""Real OSS geometry/readiness probe; select horizon mode outside this process.

Uses paused synthetic PCM, no speaker output. Exact experimental geometry is
Brook-specific; --observe permits a Linux OSS reference without assuming its
hardware-selected tuple matches Brook.
"""
import argparse
import array
import errno
import fcntl
import json
import os
import select

RESET, TRIGGER = 0x5000, 0x40045010
FRAGMENT, SPACE, BLOCK = 0xC004500A, 0x8010500C, 0xC0045004
SPEED, CHANNELS, FORMAT = 0xC0045002, 0xC0045006, 0xC0045005


def integer(fd, command, value=0):
    data = array.array('i', [value])
    fcntl.ioctl(fd, command, data, True)
    return data[0]


def space(fd):
    data = array.array('i', [0] * 4)
    fcntl.ioctl(fd, SPACE, data, True)
    return list(data)


def probe(enabled, observe):
    rows = []
    for maximum, exponent in [(4, 12), (6, 12), (2, 12), (0, 12), (69, 8), (1, 12), (47, 9)]:
        fd = os.open('/dev/dsp', os.O_WRONLY | os.O_NONBLOCK)
        try:
            assert integer(fd, SPEED, 44100) == 44100
            assert integer(fd, CHANNELS, 2) == 2
            assert integer(fd, FORMAT, 0x10) == 0x10
            integer(fd, TRIGGER, 0)
            request = (maximum << 16) | exponent
            integer(fd, FRAGMENT, request)
            initial = space(fd)
            fragment = integer(fd, BLOCK)
            assert fragment == initial[2] and fragment > 0
            assert initial[0] == initial[1] and initial[3] == initial[1] * fragment
            assert not maximum or initial[1] <= maximum
            if not observe:
                actual = {(4, 12): (4, 8192 if enabled else 4096),
                          (6, 12): (4, 8192) if enabled else (6, 4096),
                          (2, 12): (2, 32768 if enabled else 4096),
                          (0, 12): (16, 4096),
                          (69, 8): (47, 512) if enabled else (69, 256),
                          (1, 12): (1, 4096), (47, 9): (47, 512)}[(maximum, exponent)]
                assert (initial[1], fragment) == actual, (maximum, initial, enabled)
            poller = select.poll()
            poller.register(fd, select.POLLOUT)
            assert any(events & select.POLLOUT for _, events in poller.poll(0))
            written = 0
            below_threshold = None
            while written < initial[3]:
                # A paused write may admit a prefix even when a full-fragment
                # poll threshold is not met. Verify that actual boundary too.
                threshold_used = initial[3] - fragment + 4
                if written < threshold_used:
                    amount = min(fragment, threshold_used - written)
                else:
                    if below_threshold is None:
                        below_threshold = space(fd)
                        assert below_threshold[3] == fragment - 4
                        assert not any(events & select.POLLOUT for _, events in poller.poll(0))
                    amount = min(fragment, initial[3] - written)
                count = os.write(fd, bytes(amount))
                assert count > 0
                written += count
            full = space(fd)
            assert full == [0, initial[1], fragment, 0], full
            assert not any(events & select.POLLOUT for _, events in poller.poll(0))
            try:
                os.write(fd, b'\0' * 4)
            except BlockingIOError:
                pass
            else:
                raise AssertionError('full paused endpoint accepted more PCM')
            try:
                integer(fd, FRAGMENT, request)
            except OSError as error:
                assert error.errno == errno.EBUSY
            else:
                raise AssertionError('queued paused endpoint allowed geometry change')
            assert space(fd) == full
            integer(fd, RESET)
            assert space(fd) == initial
            assert integer(fd, BLOCK) == fragment
            assert any(events & select.POLLOUT for _, events in poller.poll(0))
            # Explicit renegotiation after reset, rather than resizing a live
            # or merely paused queue, must produce the same honest tuple.
            integer(fd, FRAGMENT, request)
            assert space(fd) == initial
            rows.append(dict(maximum_fragments=maximum,requested_fragment_bytes=1 << exponent,
                             actual_fragment_bytes=fragment,actual_fragments=initial[1],
                             capacity_bytes=initial[3],written_bytes=written,full=full,
                             below_threshold=below_threshold,reset_preserved_geometry=True))
        finally:
            os.close(fd)
    return dict(passed=True,horizon_expected=enabled,observe=observe,rows=rows,
                scope='Real paused/nonblocking OSS ABI and readiness; synthetic zero PCM, no playback test. '
                      'One-fragment tuple remains honestly unqualified; two large fragments are negotiated with40ms allowance. '
                      'Linux observation may differ by device.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--horizon', choices=('on', 'off'), required=True)
    parser.add_argument('--observe', action='store_true')
    args = parser.parse_args()
    print(json.dumps(probe(args.horizon == 'on', args.observe), indent=2))
