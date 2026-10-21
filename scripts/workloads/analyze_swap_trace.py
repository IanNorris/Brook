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


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.trace.read_text())
    args.output.write_text(json.dumps(result, indent=2) + '\n')
