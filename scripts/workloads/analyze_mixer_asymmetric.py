#!/usr/bin/env python3
"""Summarize independent refill opportunities with observed asymmetric geometry.

The fixture checks exact PCM and drains separately. Passing integrity does not
establish real-time progress, native browser behavior or scheduler acceptance.
"""
import argparse
import hashlib
import itertools
import json
from pathlib import Path


def analyze(path):
    text = path.read_text()
    if 'AUDIO_DELIVERY PASS' not in text or 'FAIL' in text:
        raise ValueError('Fixture integrity did not pass')
    rows, seen = [], set()
    keys = ('shape', 'mode', 'producer_period_ms', 'refresh', 'offset', 'reverse')
    for line in text.splitlines():
        if not line.startswith('MIXER_ASYMMETRIC '):
            continue
        row = {}
        for field in line.split()[1:]:
            key, value = field.split('=')
            if key in row:
                raise ValueError('Duplicate metric')
            values = [int(v) for v in value.split(',')]
            row[key] = values if len(values) > 1 else values[0]
        identity = tuple(row[k] for k in keys)
        if identity in seen:
            raise ValueError('Duplicate experiment configuration')
        seen.add(identity)
        if (row['capacities'] != [4416 if row['shape'] == 1 else 8192, 8192]
                or row['fragments'] != [64, 2048] or row['fragment'] != 1
                or row['scenario'] != 0 or row['interval_frames'] != 176400
                or row['drain_ms'] <= 6000 or row['completion'] != row['physical']
                or any(not 0 <= x <= 176400 for x in row['physical'])):
            raise ValueError('Unexpected geometry or measurement interval')
        row['source_rates'] = [x / row['interval_frames'] for x in row['physical']]
        row['playback_gate'] = (row['steady_reservations'] == 0
                                and all(.99 <= x <= 1.01 for x in row['source_rates']))
        rows.append(row)
    expected = set(itertools.product((1, 2), (0, 3), (1, 5, 10), (0, 1), (0, 1, 4), (0, 1)))
    if seen != expected:
        raise ValueError('Incomplete or unexpected asymmetric matrix')
    groups = {}
    for shape, mode, period, refresh in itertools.product((1, 2), (0, 3), (1, 5, 10), (0, 1)):
        subset = [r for r in rows if (r['shape'], r['mode'], r['producer_period_ms'], r['refresh'])
                  == (shape, mode, period, refresh)]
        groups[f'{shape}/{mode}/{period}/{refresh}'] = dict(
            cases=len(subset), failed_cases=sum(not r['playback_gate'] for r in subset),
            min_source_rates=[min(r['source_rates'][s] for r in subset) for s in (0, 1)],
            max_source_rates=[max(r['source_rates'][s] for r in subset) for s in (0, 1)],
            steady_reservations=sum(r['steady_reservations'] for r in subset),
            missing_short=sum(r['missing_short'] for r in subset),
            missing_empty=sum(r['missing_empty'] for r in subset))
    return dict(configurations=len(rows), log_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                groups=groups, rows=rows,
                scope='Actual mixer/HDA bodies, synthetic independent DMA, finite per-channel PCM. '
                'Shape1 observed Firefox4416/64 plus Quake8192/2048; shape2 increases only Firefox capacity. '
                'Firefox opportunities1/5/10ms; Quake5ms with0/1/4ms phase; both orders and refresh modes. '
                'Whole-fragment writes; no scheduler, codecs, browser pipeline, native tempo or default promotion.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    result = analyze(args.log)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: result[k] for k in ('configurations', 'groups')}, indent=2))
