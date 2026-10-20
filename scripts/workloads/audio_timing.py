#!/usr/bin/env python3
"""Bounded native-format OSS delivery timing; use only in a silent test VM.

The host must independently capture PCM and bracket clock/progress snapshots.
Guest timings alone cannot certify physical speaker timing or clock frequency.
"""
import argparse
import array
import bisect
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


def progress(fd):
    pointer = array.array('i', [0, 0, 0])
    fcntl.ioctl(fd, 0x800C5012, pointer, True)
    return pointer[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=30, choices=range(1, 46))
    parser.add_argument('--output', required=True)
    parser.add_argument('--trace-uptime', action='store_true',
                        help='Diagnostic procfs reads around writes; adds observer overhead')
    parser.add_argument('--signal',choices=['tone','packet-code-v1'],default='tone',
                        help='Packet code changes frequency every 100 ms to identify source intervals')
    parser.add_argument('--fragments',type=int,choices=range(1,17),
                        help='Optional OSS fragment-count hint; record actual negotiated geometry')
    parser.add_argument('--fragment-shift',type=int,choices=range(8,17),default=12,
                        help='Fragment byte-size exponent for --fragments (default 4096 bytes)')
    args = parser.parse_args()
    destination = Path(args.output)
    if destination.exists():
        raise SystemExit('Refusing to overwrite prior timing evidence')
    # Low amplitude; every 100-ms packet has an integer number of cycles.
    # The default remains the original 440-Hz tone (44 cycles).
    def make_packet(index):
        frequency=440 if args.signal=='tone' else 400+40*((index*137+29)%463)
        return array.array('h',[int(1200*math.sin(2*math.pi*frequency*i/44100))
                               for i in range(4410) for _ in range(2)]).tobytes()
    # Generate before opening/timing the device: producer math must not become
    # an accidental starvation workload. At most 450 packets (~8 MiB).
    packets=[make_packet(0)] if args.signal=='tone' else [make_packet(i) for i in range(args.seconds*10)]
    rows = []
    submitted = 0
    failure = None
    submitted_at = completed_at = final_bytes = None
    started = time.monotonic_ns()
    attempt = None
    geometry = None
    def uptime():
        if not args.trace_uptime:
            return None
        return Path('/proc/uptime').read_text().split()[0]
    fd = os.open('/dev/dsp', os.O_WRONLY)
    try:
        for request, value in ((0xC0045005, 16), (0xC0045006, 2), (0xC0045002, 44100)):
            negotiated = integer(fd, request, value)
            if negotiated != value:
                raise RuntimeError(('format negotiation', request, value, negotiated))
        hint = (args.fragments << 16) | args.fragment_shift if args.fragments else None
        returned_hint = integer(fd, 0xC004500A, hint) if hint is not None else None
        space = array.array('i', [0, 0, 0, 0])
        fcntl.ioctl(fd, 0x8010500C, space, True)  # SNDCTL_DSP_GETOSPACE
        geometry = dict(requested_hint=hint, returned_hint=returned_hint,
                        available_fragments=space[0], total_fragments=space[1],
                        fragment_bytes=space[2], available_bytes=space[3])
        print('AUDIO_TIMING_GEOMETRY', json.dumps(geometry), flush=True)
        started = time.monotonic_ns()
        print('AUDIO_TIMING_STARTED', started, flush=True)
        for packet_index in range(args.seconds * 10):
            packet=packets[0] if args.signal=='tone' else packets[packet_index]
            offset = 0
            while offset < len(packet):
                attempt = dict(before_ns=time.monotonic_ns(), uptime_before=uptime(),
                               requested_bytes=len(packet)-offset)
                accepted = os.write(fd, packet[offset:])
                attempt.update(after_ns=time.monotonic_ns(), uptime_after=uptime(),
                               accepted_bytes=accepted)
                if not 0 < accepted <= len(packet)-offset or accepted % 4:
                    raise RuntimeError(('invalid native write result', accepted))
                offset += accepted
                submitted += accepted
            before = time.monotonic_ns()
            completed = progress(fd)
            after = time.monotonic_ns()
            rows.append(dict(packet_index=packet_index,before_ns=before, after_ns=after,
                             completed_bytes=completed, submitted_bytes=submitted,
                             write=attempt))
        submitted_at = time.monotonic_ns()
        fcntl.ioctl(fd, 0x5001)  # SNDCTL_DSP_SYNC, not acceptance alone.
        completed_at = time.monotonic_ns()
        final_bytes = progress(fd)
        if final_bytes != submitted:
            raise RuntimeError(('completion mismatch', final_bytes, submitted))
    except Exception as error:
        completed_at = time.monotonic_ns()
        failure = dict(error=repr(error), errno=getattr(error, 'errno', None),
                       observed_ns=completed_at, uptime_after=uptime(),
                       fd_flags=fcntl.fcntl(fd, fcntl.F_GETFL), write=attempt)
        try:
            failure['completed_bytes'] = progress(fd)
            failure['delay_bytes'] = integer(fd, 0x80045017, 0)
        except OSError as inspection_error:
            failure['inspection_error'] = repr(inspection_error)
    finally:
        os.close(fd)
    windows = []
    stamps = [row['after_ns'] for row in rows]
    for index, row in enumerate(rows):
        previous = bisect.bisect_right(stamps, row['before_ns']-4_000_000_000)-1
        if previous < 0 or previous >= index:
            continue
        first = rows[previous]
        frames = (row['completed_bytes']-first['completed_bytes'])/4
        minimum = row['before_ns']-first['after_ns']
        maximum = row['after_ns']-first['before_ns']
        windows.append(dict(completed_frames=frames,
                            minimum_interval_ns=minimum, maximum_interval_ns=maximum,
                            rate_ratio_low=frames*1e9/(maximum*44100),
                            rate_ratio_high=frames*1e9/(minimum*44100)))
    result = dict(requested_seconds=args.seconds, sample_rate=44100, channels=2,signal=args.signal,
                  oss_geometry=geometry,
                  signal_code=dict(packet_frames=4410,base_hz=400,step_hz=40,modulus=463,multiplier=137,offset=29) if args.signal!='tone' else None,
                  submitted_frames=submitted//4,
                  completed_frames=final_bytes//4 if final_bytes is not None else None,
                  start_ns=started, submitted_at_ns=submitted_at, completed_at_ns=completed_at,
                  delivery_guest_seconds=(completed_at-started)/1e9,
                  completion_windows=windows, observations=rows, failure=failure,
                  trace_uptime=args.trace_uptime)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(dict(output=str(destination), requested_seconds=args.seconds,signal=args.signal,
                          completed_frames=result['completed_frames'], failure=failure,
                          delivery_guest_seconds=result['delivery_guest_seconds'],
                          max_4s_completion_ratio_low=max((x['rate_ratio_low'] for x in windows), default=None),
                          min_4s_completion_ratio_high=min((x['rate_ratio_high'] for x in windows), default=None))), flush=True)
    return 1 if failure else 0


if __name__ == '__main__':
    raise SystemExit(main())
