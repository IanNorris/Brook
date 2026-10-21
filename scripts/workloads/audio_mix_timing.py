#!/usr/bin/env python3
"""Finite nonblocking OSS mixing probe; channels identify independent streams.

Run in a private VM with no other audio streams. Source generation precedes
opening devices. Host PCM/clock brackets remain separate acceptance evidence.
"""
import argparse
import array
import errno
import fcntl
import json
import math
import os
from pathlib import Path
import time


def integer(fd, request, value):
    argument = array.array('i', [value])
    fcntl.ioctl(fd, request, argument, True)
    return argument[0]


def completed(fd):
    pointer = array.array('i', [0, 0, 0])
    fcntl.ioctl(fd, 0x800C5012, pointer, True)
    return pointer[0]


def source(seconds, channel):
    samples = array.array('h')
    for packet in range(seconds * 10):
        frequency = 400 + 40 * ((packet * 137 + 29) % 463)
        for frame in range(4410):
            value = int(1200 * math.sin(2 * math.pi * frequency * frame / 44100))
            samples.extend((value, 0) if channel == 0 else (0, value))
    return samples.tobytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=12, choices=range(8, 21))
    parser.add_argument('--channels', choices=('left', 'right', 'both'), default='both')
    parser.add_argument('--first', type=int, choices=(0, 1), default=0,
                        help='Initial descriptor refill order; alternates each loop')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('Refusing to overwrite prior evidence')
    channels = [0, 1] if args.channels == 'both' else [0 if args.channels == 'left' else 1]
    payloads = [source(args.seconds, channel) for channel in channels]
    views = [memoryview(payload) for payload in payloads]
    devices, geometry, writes, observations = [], [], [], []
    submitted = [0] * len(channels)
    played = [0] * len(channels)
    eagain = [0] * len(channels)
    partial = [0] * len(channels)
    started = finished = None
    failure = None
    try:
        for channel in channels:
            fd = os.open('/dev/dsp', os.O_WRONLY | os.O_NONBLOCK)
            devices.append(fd)
            for request, value in ((0xC0045005, 16), (0xC0045006, 2), (0xC0045002, 44100)):
                if integer(fd, request, value) != value:
                    raise RuntimeError(('format negotiation', request, value))
            hint = (4 << 16) | 12
            returned = integer(fd, 0xC004500A, hint)
            space = array.array('i', [0, 0, 0, 0])
            fcntl.ioctl(fd, 0x8010500C, space, True)
            row = dict(channel=channel, requested_hint=hint, returned_hint=returned,
                       available_fragments=space[0], total_fragments=space[1],
                       fragment_bytes=space[2], available_bytes=space[3])
            geometry.append(row)
            if space[1] != 4 or space[2] != 4096:
                raise RuntimeError(('geometry mismatch', row))
        started = time.monotonic_ns()
        print('AUDIO_MIX_READY', json.dumps(dict(start_ns=started, channels=channels,
              frames_each=args.seconds * 44100, geometry=geometry)), flush=True)
        deadline = started + 90_000_000_000
        next_observation = started
        order = args.first % len(devices)
        while True:
            changed = False
            for index in [(order + i) % len(devices) for i in range(len(devices))]:
                if submitted[index] == len(payloads[index]):
                    continue
                offset = submitted[index]
                count = min(4096, len(payloads[index]) - offset)
                before = time.monotonic_ns()
                try:
                    accepted = os.write(devices[index], views[index][offset:offset+count])
                except OSError as error:
                    if error.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                        raise
                    eagain[index] += 1
                    continue
                after = time.monotonic_ns()
                if not 0 < accepted <= count or accepted % 4:
                    raise RuntimeError(('invalid write result', index, accepted, count))
                partial[index] += accepted < count
                submitted[index] += accepted
                writes.append(dict(stream=index, channel=channels[index], offset_bytes=offset,
                                   requested_bytes=count, accepted_bytes=accepted,
                                   before_ns=before, after_ns=after))
                changed = True
            order = (order + 1) % len(devices)
            now = time.monotonic_ns()
            if now >= deadline:
                raise TimeoutError('Retain original finite dual-stream source; ninety-second guest deadline')
            if now >= next_observation:
                row = dict(before_ns=time.monotonic_ns(), streams=[])
                for index, fd in enumerate(devices):
                    before = time.monotonic_ns()
                    count = completed(fd)
                    after = time.monotonic_ns()
                    if count < played[index] or count > submitted[index] or count % 4:
                        raise RuntimeError(('invalid completion', index, count, played[index], submitted[index]))
                    played[index] = count
                    row['streams'].append(dict(stream=index, channel=channels[index],
                                               before_ns=before, after_ns=after,
                                               submitted_bytes=submitted[index], completed_bytes=count))
                row['after_ns'] = time.monotonic_ns()
                observations.append(row)
                next_observation = row['after_ns'] + 100_000_000
                if all(played[i] == len(payloads[i]) for i in range(len(devices))):
                    break
            if not changed:
                time.sleep(.001)
        for index, fd in enumerate(devices):
            fcntl.ioctl(fd, 0x5001)  # Explicit final OSS sync after observed exact drain.
            if completed(fd) != len(payloads[index]):
                raise RuntimeError(('final sync mismatch', index))
        finished = time.monotonic_ns()
    except Exception as error:
        finished = time.monotonic_ns()
        failure = dict(error=repr(error), errno=getattr(error, 'errno', None))
    finally:
        for fd in devices:
            os.close(fd)
    report = dict(schema=1, channels=channels, requested_seconds=args.seconds,
                  frames_each=args.seconds * 44100, rate=44100, stereo=True,
                  geometry=geometry, first=args.first, start_ns=started, end_ns=finished,
                  duration_guest_seconds=(finished-started)/1e9 if started is not None else None,
                  submitted_bytes=submitted, completed_bytes=played, eagain=eagain,
                  partial_writes=partial, writes=writes, observations=observations,
                  failure=failure, scope='One coordinator alternates nonblocking refills; all samples pregenerated. Channels identify contributions. No independent-writer or scheduling-fairness claim; source/device clocks and physical tempo require host evidence.')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print('AUDIO_MIX_RESULT', json.dumps(dict(output=str(args.output), failure=failure,
          completed_bytes=played, duration_guest_seconds=report['duration_guest_seconds'])), flush=True)
    return 1 if failure else 0


if __name__ == '__main__':
    raise SystemExit(main())
