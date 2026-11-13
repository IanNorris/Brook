#!/usr/bin/env python3
"""Measure overlap, not causation, between saved swap and compositor intervals."""
import argparse
import csv
import json
import math
from pathlib import Path
import re


def merge(intervals):
    result = []
    for start, end in sorted(intervals):
        if end <= start:
            continue
        if result and start <= result[-1][1]:
            result[-1] = (result[-1][0], max(result[-1][1], end))
        else:
            result.append((start, end))
    return result


def overlap(start, end, spans):
    return sum(max(0, min(end, b) - max(start, a)) for a, b in spans)


def analyze(swap_path, renderer_path, timing_path):
    timing = json.loads(timing_path.read_text())
    start = timing['markers']['START']['after_ns']
    end = timing['markers']['END']['after_ns']
    if start <= 0 or end <= start:
        raise ValueError('Invalid benchmark marker interval')
    with swap_path.open() as stream:
        header = stream.readline().strip()
        if not header.startswith('# brook_swap_trace_v1 '):
            raise ValueError('Unknown swap trace')
        counters = {k: int(v) for k, v in re.findall(r'(count|dropped|incomplete|invalid_clock)=(\d+)', header)}
        if set(counters) != {'count', 'dropped', 'incomplete', 'invalid_clock'}:
            raise ValueError('Incomplete swap trace header')
        swaps = [(int(row['begin_ns']), int(row['end_ns'])) for row in csv.DictReader(stream)]
    if len(swaps) != counters['count'] or any(a <= 0 or b < a for a, b in swaps):
        raise ValueError('Invalid swap count or clock')
    spans = merge((max(a, start), min(b, end)) for a, b in swaps)
    rows, summary = [], None
    for line in renderer_path.read_text(errors='replace').splitlines():
        if line.startswith('DESKTOP_TIMELINE_SUMMARY '):
            summary = dict(re.findall(r'(\w+)=([^\s]+)', line))
        if not line.startswith('DESKTOP_TIMELINE frame='):
            continue
        row = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)}
        fields = ('start', 'submit_start', 'submitted', 'completed', 'presented', 'finish')
        if not row.get('window'):
            continue
        times = [row[x] for x in fields]
        if not all(times) or times != sorted(times):
            raise ValueError('Invalid renderer clocks')
        if start <= times[0] and times[-1] <= end:
            rows.append(row)
    rows.sort(key=lambda row: row['start'])
    if any(b['start'] < a['finish'] for a, b in zip(rows, rows[1:])):
        raise ValueError('Overlapping frames: summed stage intervals would double-count')
    stages = {}
    for label, first, last in [('preparation', 'start', 'submit_start'), ('submit', 'submit_start', 'submitted'),
                               ('completion_wait', 'submitted', 'completed'), ('present', 'completed', 'presented'),
                               ('finish', 'presented', 'finish')]:
        durations = sorted(row[last] - row[first] for row in rows)
        stages[label] = {'total_ns': sum(durations),
                         'swap_overlap_ns': sum(overlap(row[first], row[last], spans) for row in rows),
                         'p50_ns': durations[math.ceil(.5 * len(durations)) - 1] if durations else None}
    return {'marker_interval_ns': end - start, 'swap_trace': counters,
            'swap_calls_completed_in_markers': sum(start <= b <= end for _, b in swaps),
            'swap_union_ns': sum(b - a for a, b in spans), 'direct_rows_fully_inside_markers': len(rows),
            'renderer_summary': summary, 'stages': stages,
            'scope': 'CPU-observed overlapping intervals; overlap does not prove a wait dependency. '
                     'Marker polling and retained-row drops limit completeness; excludes scanout.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--swaps', type=Path, required=True)
    parser.add_argument('--renderer', type=Path, required=True)
    parser.add_argument('--timing', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(analyze(args.swaps, args.renderer, args.timing), indent=2))


if __name__ == '__main__':
    main()
