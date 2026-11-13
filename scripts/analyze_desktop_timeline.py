#!/usr/bin/env python3
"""Summarize paired Brook desktop timeline/detail rows from a retained log.

CPU durations partition render start through finish. GPU query duration is an
overlapping interval, not an extra stage; it can include barriers and scheduling.
This does not measure physical scanout or time before render start.
"""
import argparse
import json
import math
from pathlib import Path


def analyze(text, marker_interval=None):
    if marker_interval is not None and (len(marker_interval) != 2 or
            marker_interval[0] <= 0 or marker_interval[1] <= marker_interval[0]):
        raise ValueError('Invalid marker interval')
    timelines, details = {}, {}
    summary = None
    duplicates = 0
    for line in text.splitlines():
        pieces = line.split()
        if not pieces:
            continue
        kind = pieces[0]
        if kind not in ('DESKTOP_TIMELINE', 'DESKTOP_TIMELINE_DETAIL', 'DESKTOP_TIMELINE_SUMMARY'):
            continue
        fields = dict(piece.split('=', 1) for piece in pieces[1:] if '=' in piece)
        if kind == 'DESKTOP_TIMELINE_SUMMARY':
            summary = fields
            continue
        try:
            row = {key: int(value) for key, value in fields.items()}
            key = (row['frame'], row['ticket'])
        except (ValueError, KeyError):
            continue
        target = timelines if kind == 'DESKTOP_TIMELINE' else details
        if key in target:
            duplicates += 1
        target[key] = row
    stages = {'channel_poll': ('start', 'channel'), 'query_and_setup': ('channel', 'query'),
              'client_capture_and_selection': ('query', 'clients'), 'backdrop': ('clients', 'backdrop'),
              'chrome': ('backdrop', 'chrome'), 'overlay': ('chrome', 'overlay'),
              'prepare_after_overlay': ('overlay', 'submit_start'),
              'submit': ('submit_start', 'submitted'), 'completion_wait': ('submitted', 'completed'),
              'present': ('completed', 'presented'), 'finish': ('presented', 'finish'),
              'total': ('start', 'finish')}
    measurements = {name: [] for name in stages}
    measurements['gpu_interval'] = []
    invalid = 0
    outside = 0
    paths = {}
    groups = {}
    for key in sorted(timelines.keys() & details.keys()):
        a, b = timelines[key], details[key]
        merged = dict(a, **b)
        try:
            times = [merged[x] for x in ('start', 'channel', 'query', 'clients', 'backdrop', 'chrome', 'overlay',
                                         'submit_start', 'submitted', 'completed', 'presented', 'finish')]
            if not all(times) or times != sorted(times):
                raise ValueError('invalid timestamps')
            if marker_interval is not None and (times[0] < marker_interval[0] or times[-1] > marker_interval[1]):
                outside += 1
                continue
            path = (merged['graphics'], merged['partial'], merged['pixels'], merged['layers'], merged['regions'])
            if path[0] not in (0, 1) or path[1] not in (0, 1) or min(path[2:]) <= 0:
                raise ValueError('invalid geometry')
            for name, (begin, end) in stages.items():
                measurements[name].append(merged[end] - merged[begin])
            if merged.get('gpu_valid') == 1:
                measurements['gpu_interval'].append(merged['gpu_ns'])
            label = 'graphics=%d partial=%d pixels=%d layers=%d regions=%d' % path
            paths[label] = paths.get(label, 0) + 1
            group = groups.setdefault(label, {name: [] for name in measurements})
            for name, (begin, finish) in stages.items():
                group[name].append(merged[finish] - merged[begin])
            if merged.get('gpu_valid') == 1:
                group['gpu_interval'].append(merged['gpu_ns'])
        except (KeyError, ValueError):
            invalid += 1
    def stats(values):
        if not values:
            return None
        values.sort()
        return {'n': len(values), 'p50_ns': values[math.ceil(.50 * len(values)) - 1],
                'p95_ns': values[math.ceil(.95 * len(values)) - 1], 'max_ns': values[-1]}
    return {'timeline_rows': len(timelines), 'detail_rows': len(details),
            'unpaired_timeline': len(timelines.keys() - details.keys()),
            'unpaired_detail': len(details.keys() - timelines.keys()), 'duplicate_rows': duplicates,
            'invalid_rows': invalid, 'outside_markers': outside, 'marker_interval_ns': marker_interval,
            'summary': summary, 'paths': paths,
            'stages': {name: stats(values) for name, values in measurements.items()},
            'stages_by_path': {path: {name: stats(values) for name, values in group.items()}
                               for path, group in groups.items()},
            'scope': 'Presented frames retained by renderer; CPU intervals partition start to finish; '
                     'GPU interval overlaps CPU wait and is not physical scanout.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--timing', type=Path, help='Use frames fully inside demo marker observations')
    args = parser.parse_args()
    markers = None
    if args.timing:
        timing = json.loads(args.timing.read_text())
        markers = (timing['markers']['START']['after_ns'], timing['markers']['END']['after_ns'])
    print(json.dumps(analyze(args.log.read_text(errors='replace'), markers), indent=2))


if __name__ == '__main__':
    main()
