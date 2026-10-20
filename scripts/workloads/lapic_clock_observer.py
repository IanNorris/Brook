#!/usr/bin/env python3
"""Boundedly compare Brook's legacy /proc/uptime ticks with monotonic time.

Read windows and 10-ms procfs quantization are retained in anomaly evidence.
This does not assert that Linux /proc/uptime is derived from a LAPIC counter.
"""
import argparse
import json
import time


def sample(brook_centiseconds=False):
    before = time.monotonic_ns()
    with open('/proc/uptime') as stream:
        value = stream.read().split()[0]
    after = time.monotonic_ns()
    whole, _, fraction = value.partition('.')
    if brook_centiseconds:
        if not fraction.isdigit() or not 1 <= len(fraction) <= 2:
            raise ValueError(('unexpected Brook centisecond field', value))
        fractional_ns = int(fraction) * 10_000_000
    else:
        fractional_ns = int((fraction+'000000000')[:9])
    uptime = int(whole) * 1_000_000_000 + fractional_ns
    return dict(before_ns=before, after_ns=after, uptime_ns=uptime,
                offset_ns=uptime-(before+after)//2, uptime_text=value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=24, choices=range(1, 61))
    parser.add_argument('--brook-centiseconds', action='store_true',
                        help='Decode the known legacy Brook unpadded centisecond field')
    args = parser.parse_args()
    initial = previous = sample(args.brook_centiseconds)
    samples = backward = jumps = 0
    largest_read = largest_shift = 0
    anomalies = []
    while True:
        time.sleep(.002)
        current = sample(args.brook_centiseconds)
        samples += 1
        width = current['after_ns']-current['before_ns']
        largest_read = max(largest_read, width)
        shift = current['offset_ns']-previous['offset_ns']
        largest_shift = max(largest_shift, abs(shift))
        went_back = current['uptime_ns'] < previous['uptime_ns']
        # A 250-ms change is well above procfs's 10-ms quantization. Include
        # both read-window widths rather than treating the midpoint as exact.
        uncertain = width + previous['after_ns']-previous['before_ns']
        jumped = abs(shift) > 250_000_000 + uncertain
        backward += went_back
        jumps += jumped
        if (went_back or jumped) and len(anomalies) < 20:
            anomalies.append(dict(previous=previous, current=current, offset_shift_ns=shift,
                                  backwards=went_back, exceeds_window_and_quantization=jumped))
        previous = current
        if current['after_ns']-initial['before_ns'] >= args.seconds*1_000_000_000:
            break
    print(json.dumps(dict(samples=samples, backwards=backward, large_jumps=jumps,
                         max_read_ns=largest_read, max_offset_shift_ns=largest_shift,
                         first=initial, last=previous, anomalies=anomalies)), flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
