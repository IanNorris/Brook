#!/usr/bin/env python3
"""Synthetic exact-sample tail test for a complete silent-QEMU capture window.

The caller must begin a ten-second capture during the three-second lead-in.
Compare nonzero left-channel samples against expected_samples(): silence gaps
are deliberately excluded because QEMU wavcapture omits inactive intervals.
"""
import array
import fcntl
import json
import os
import sys
import time

SIZES = (1, 16, 128, 441, 1024)
REPEATS = 6


def expected_samples():
    samples = []
    for _ in range(REPEATS):
        for index, frames in enumerate(SIZES):
            magnitude = 4000 + index * 2000
            samples.extend([magnitude] * (frames - 1) + [-magnitude])
    return samples


def queued(fd):
    value = array.array('i', [0])
    fcntl.ioctl(fd, 0x80045017, value, True)
    return value[0]


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else 'persistent'
    if mode not in ('persistent', 'close', 'reset'):
        raise ValueError('Choose persistent, close or reset')
    fd = None
    try:
        if mode != 'close':
            fd = os.open('/dev/dsp', os.O_WRONLY)
        print('SHORT_TAIL_READY', flush=True)
        time.sleep(3)
        for _ in range(REPEATS):
            for index, frames in enumerate(SIZES):
                if fd is None:
                    fd = os.open('/dev/dsp', os.O_WRONLY)
                magnitude = 4000 + index * 2000
                mono = [magnitude] * (frames - 1) + [-magnitude]
                stereo = array.array('h', [sample for sample in mono for _ in range(2)]).tobytes()
                assert os.write(fd, stereo) == len(stereo)
                deadline = time.monotonic() + 2
                while queued(fd):
                    assert time.monotonic() < deadline, 'DMA did not consume sound'
                    time.sleep(0.001)
                if mode == 'close':
                    os.close(fd)
                    fd = None
                elif mode == 'reset':
                    fcntl.ioctl(fd, 0x5000)
                time.sleep(0.075)
        print('SHORT_TAIL_PASS', json.dumps({'mode': mode, 'sizes': SIZES,
              'repeats': REPEATS, 'source_frames': len(expected_samples())}), flush=True)
    finally:
        if fd is not None:
            os.close(fd)


if __name__ == '__main__':
    main()
