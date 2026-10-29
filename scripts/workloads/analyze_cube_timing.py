#!/usr/bin/env python3
"""Analyze bounded cube timestamps without adding overlapping GPU/CPU totals."""
import argparse
import csv
import io
import json
import math
import re
from pathlib import Path


def analyze(text, start_ns=None, end_ns=None):
    window = start_ns is not None or end_ns is not None
    if window and (type(start_ns) is not int or type(end_ns) is not int or
                   not 0 <= start_ns < end_ns < (1 << 64)):
        raise ValueError('window requires ordered uint64 start/end timestamps')
    header, body = text.split('\n', 1)
    match = re.fullmatch(r'# bits=(\d+) period_ns=(\S+) errors=(\d+) overflow=(\d+)', header)
    if not match:
        raise ValueError('missing trace metadata')
    bits, period, errors, overflow = match.groups()
    bits, period = int(bits), float(period)
    if not 1 <= bits <= 64 or not math.isfinite(period) or period <= 0 or int(errors) or int(overflow):
        raise ValueError('invalid, incomplete or overflowing capture')
    stages = ['cycle_start_ns', 'after_wait_ns', 'after_acquire_ns', 'before_submit_ns',
              'after_submit_ns', 'before_present_ns', 'after_present_ns']
    rows = list(csv.DictReader(io.StringIO(body)))
    if len(rows) < 2:
        raise ValueError('too few frames')
    output = []
    previous = None
    mask = (1 << bits) - 1
    for index, raw in enumerate(rows):
        row = {k: int(v) for k, v in raw.items()}
        if row['frame'] != index or row['submitted'] != 1 or row['ready'] != 1 or any(not 0 <= v < 1 << 64 for v in row.values()):
            raise ValueError('missing frame or unavailable GPU result')
        cpu = [row[s] for s in stages]
        if cpu != sorted(cpu) or (previous and cpu[0] < previous['after_present_ns']):
            raise ValueError('CPU clock/order invalid')
        ticks = (row['gpu_end'] - row['gpu_start']) & mask
        if ticks >= 1 << (bits - 1):
            raise ValueError('reversed GPU time or ambiguous wrap')
        frame = dict(frame=index, gpu_interval_ms=ticks * period / 1e6,
            fence_wait_and_query_read_ms=(cpu[1] - cpu[0]) / 1e6,
            acquire_ms=(cpu[2] - cpu[1]) / 1e6,
            prepare_ms=(cpu[3] - cpu[2]) / 1e6,
            submit_ms=(cpu[4] - cpu[3]) / 1e6,
            pre_present_ms=(cpu[5] - cpu[4]) / 1e6,
            present_ms=(cpu[6] - cpu[5]) / 1e6,
            draw_call_ms=(cpu[6] - cpu[0]) / 1e6)
        if previous:
            frame['between_draw_calls_ms'] = (cpu[0] - previous['after_present_ns']) / 1e6
            frame['present_return_interval_ms'] = (cpu[6] - previous['after_present_ns']) / 1e6
        # Validate the entire trace, including frames outside the requested
        # window. Keep only intervals whose two presentation returns are inside.
        if not window or (previous is not None and
                          start_ns <= previous['after_present_ns'] and cpu[6] <= end_ns):
            output.append(frame)
        previous = row
    if not output:
        raise ValueError('no complete presentation interval inside window')
    summaries = {}
    for key in output[-1]:
        if key == 'frame': continue
        values = sorted(r[key] for r in output if key in r)
        summaries[key] = {p: values[math.ceil(q * len(values)) - 1] for p, q in [('p50', .5), ('p95', .95), ('p99', .99), ('max', 1)]}
    return dict(frames=len(output), captured_frames=len(rows),
        window_ns=dict(start=start_ns,end=end_ns) if window else None, summary_ms=summaries,
        longest_present_intervals=sorted((r for r in output if 'present_return_interval_ms' in r), key=lambda r: r['present_return_interval_ms'], reverse=True)[:10],
        scope='CPU wall stages include descheduling; GPU interval includes dependencies and semaphore waits. GPU and CPU overlap. Present return is not physical scanout. Uncalibrated GPU durations only.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--start-ns',type=int)
    parser.add_argument('--end-ns',type=int)
    args = parser.parse_args()
    print(json.dumps(analyze(args.trace.read_text(),args.start_ns,args.end_ns), indent=2))
