#!/usr/bin/env python3
"""Analyze complete single-thread SDL swap traces, with explicit clock scope."""
import argparse
import csv
import json
import math
from pathlib import Path
import re


def distribution(values):
    ordered = sorted(values)
    return {name: ordered[max(0, math.ceil(q * len(ordered)) - 1)] / 1e6
            for name, q in [('p50_ms', .5), ('p90_ms', .9), ('p95_ms', .95), ('max_ms', 1)]}


def analyze(text):
    lines = text.splitlines()
    match = re.fullmatch(r'# brook_swap_trace_v1 count=(\d+) dropped=(\d+) incomplete=(\d+) invalid_clock=(\d+)',
                         lines[0] if lines else '')
    if not match:
        raise ValueError('Missing trace quality header')
    count, dropped, incomplete, invalid = map(int, match.groups())
    if dropped or incomplete or invalid:
        raise ValueError('Dropped, incomplete or invalid-clock frames')
    reader = csv.DictReader(lines[1:])
    if reader.fieldnames != ['index', 'tid', 'begin_ns', 'end_ns']:
        raise ValueError('Invalid trace columns')
    rows = [{k: int(v) for k, v in row.items()} for row in reader]
    if len(rows) != count or count < 2:
        raise ValueError('Incomplete or insufficient frame records')
    if len({r['tid'] for r in rows}) != 1 or rows[0]['tid'] <= 0:
        raise ValueError('Multiple or invalid swap thread identities')
    for i, row in enumerate(rows):
        if row['index'] != i or not 0 < row['begin_ns'] <= row['end_ns']:
            raise ValueError('Invalid frame sequence or clock sample')
        if i and row['begin_ns'] < rows[i-1]['end_ns']:
            raise ValueError('Overlapping swaps or regressing clock')
    intervals = [b['end_ns'] - a['end_ns'] for a, b in zip(rows, rows[1:])]
    if min(intervals) <= 0:
        raise ValueError('Non-increasing frame completion timestamps')
    return dict(frames=count, intervals=len(intervals),
                frame_time=distribution(intervals),
                swap_call_time=distribution([r['end_ns']-r['begin_ns'] for r in rows]),
                stalls={str(ms)+'ms': sum(v > ms*1000000 for v in intervals)
                        for ms in (50, 100, 250, 1000)},
                average_fps=len(intervals)*1e9/sum(intervals),
                clock='guest CLOCK_MONOTONIC; validate against host separately',
                scope='SDL swap completion cadence including frame pacing; swap call duration '
                      'includes blocking. Not compositor presentation or physical scanout. '
                      'Whole capture includes startup/menu unless explicitly segmented.')


def analyze_window(text, start_ns, end_ns):
    """Retain boundary-crossing stalls instead of dropping them when cropping.

    Both bounds must use the trace's guest clock domain. Host load containment
    and the host/guest clock relationship are separate caller-owned gates.
    """
    analyze(text)  # Validate the complete source, including quality counters.
    if not 0 < start_ns < end_ns:
        raise ValueError('Invalid window bounds')
    rows = [{k: int(v) for k, v in row.items()}
            for row in csv.DictReader(text.splitlines()[1:])]
    if rows[0]['end_ns'] > start_ns or rows[-1]['end_ns'] < end_ns:
        raise ValueError('Trace does not cover the whole requested window')
    intervals = []
    crossing = []
    for before, after in zip(rows, rows[1:]):
        lo, hi = before['end_ns'], after['end_ns']
        if hi <= start_ns or lo >= end_ns:
            continue
        if start_ns <= lo and hi <= end_ns:
            intervals.append(hi-lo)
        else:
            crossing.append(dict(begin_ns=lo, end_ns=hi, gap_ns=hi-lo,
                                 overlap_ns=min(hi, end_ns)-max(lo, start_ns)))
    completed = sum(start_ns < r['end_ns'] <= end_ns for r in rows)
    return dict(start_ns=start_ns, end_ns=end_ns, completed_swaps=completed,
                completion_rate_per_guest_second=completed*1e9/(end_ns-start_ns),
                complete_intervals=len(intervals),
                frame_time=distribution(intervals) if intervals else None,
                boundary_crossing_intervals=crossing,
                scope='Guest-clock window; distribution excludes boundary-crossing intervals '
                      'which remain explicit. No completed swap is not a zero-duration frame. '
                      'Not scanout or independent proof of host-load overlap.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.trace.read_text())
    args.output.write_text(json.dumps(result, indent=2) + '\n')
