#!/usr/bin/env python3
"""Summarize the hosted actual-body mixer commitment experiment.

Integrity checks belong to the C++ fixture. Playback acceptance is deliberately
separate: passing drains/sample ordering does not make a batching policy pass.
"""
import argparse
import hashlib
import itertools
import json
from pathlib import Path


def analyze(log):
    text = log.read_text()
    if 'AUDIO_DELIVERY PASS' not in text or 'FAIL' in text:
        raise ValueError('host fixture did not pass its integrity checks')
    rows = []
    keys = ('mode', 'capacity', 'fragment_frames', 'fragment', 'refresh', 'offset', 'reverse', 'scenario')
    seen = set()
    for line in text.splitlines():
        if not line.startswith('MIXER_STEADY '):
            continue
        row = {}
        for field in line.split()[1:]:
            key, value = field.split('=')
            if key in row:
                raise ValueError('duplicate metric')
            values = [int(v) for v in value.split(',')]
            row[key] = values if len(values) > 1 else values[0]
        row.setdefault('fragment_frames', 1024)  # Earlier fixed-fragment matrices.
        identity = tuple(row[k] for k in keys)
        if identity in seen:
            raise ValueError('duplicate experiment configuration')
        seen.add(identity)
        if (row['interval_frames'] != 176400 or row['drain_ms'] <= 6000 or
                row['completion'] != row['physical'] or
                any(not 0 <= value <= 176400 for value in row['physical'])):
            raise ValueError('invalid central measurement interval')
        row['source_rates'] = [value / row['interval_frames'] for value in row['completion']]
        row['regular_playback_gate'] = (
            row['scenario'] == 0 and row['steady_reservations'] == 0 and
            all(.99 <= rate <= 1.01 for rate in row['source_rates']))
        rows.append(row)
    expected = set(itertools.product(range(3), (4096, 8192), (1024,), range(2), range(2),
                                     (0, 1, 4), range(2), (0,)))
    expected.update(itertools.product(range(3), (4096,), (1024,), (1,), (1,), (1,), range(2), (1, 2)))
    horizon_expected = set(itertools.product(range(4), (4096, 5120, 6144, 8192), (1024,), range(2), range(2),
                                             (0, 1, 4), range(2), (0,)))
    horizon_expected.update(itertools.product(range(4), (4096,), (1024,), (1,), (1,), (1,), range(2), (1, 2)))
    horizon_expected.update(itertools.product((3,), (5120, 6144, 8192), (1024,), (1,), (1,), (1,), range(2), (1, 2)))
    geometry_expected = horizon_expected | set(itertools.product((3,), (6144, 8192), (2048,), range(2), range(2), (0, 1, 4), range(2), (0,)))
    geometry_expected.update(itertools.product((3,), (6144, 8192), (2048,), (1,), (1,), (1,), range(2), (1, 2)))
    if seen not in (expected, horizon_expected, geometry_expected):
        raise ValueError('incomplete or unexpected experiment matrix')
    candidates = [row for row in rows if row['mode'] == 2 and row['scenario'] == 0]
    horizons = [row for row in rows if row['mode'] == 3 and row['scenario'] == 0]
    by_capacity = {}
    by_geometry = {}
    for capacity in sorted({row['capacity'] for row in horizons}):
        regular = [row for row in horizons if row['capacity'] == capacity]
        by_capacity[capacity] = dict(
            cases=len(regular), failed_cases=sum(not row['regular_playback_gate'] for row in regular),
            fragment_frames=sorted({row['fragment_frames'] for row in regular}),
            min_source_rate=min(min(row['source_rates']) for row in regular),
            max_source_rate=max(max(row['source_rates']) for row in regular),
            steady_reservations=sum(row['steady_reservations'] for row in regular))
    for capacity, fragment in sorted({(row['capacity'], row['fragment_frames']) for row in horizons}):
        regular = [row for row in horizons if (row['capacity'], row['fragment_frames']) == (capacity, fragment)]
        by_geometry[f'{capacity}/{fragment}'] = dict(
            cases=len(regular), failed_cases=sum(not row['regular_playback_gate'] for row in regular),
            min_source_rate=min(min(row['source_rates']) for row in regular),
            max_source_rate=max(max(row['source_rates']) for row in regular),
            steady_reservations=sum(row['steady_reservations'] for row in regular),
            over_target_samples=(sum(row['over_horizon_samples'] for row in regular)
                                 if all('over_horizon_samples' in row for row in regular) else None))
    return dict(
        log_sha256=hashlib.sha256(log.read_bytes()).hexdigest(), configurations=len(rows),
        candidate_policy='host-only guarded441-frame quantum cap',
        candidate_regular_cases=len(candidates),
        candidate_failed_regular_cases=sum(not row['regular_playback_gate'] for row in candidates),
        candidate_playback_accepted=all(row['regular_playback_gate'] for row in candidates),
        horizon_by_capacity=by_capacity,
        horizon_by_geometry=by_geometry,
        rows=rows,
        scope='Hosted mixer/HDA bodies with synthetic MMIO/DMA and deterministic producer opportunities. '
              'Fixed central2–6s interval,44.1kHz,full PCM ordering/exact source drains checked by C++ fixture. '
              'Passing integrity does not establish playback acceptance. No kernel scheduler, host audio, '
              'browser/game behavior or native timing validation; no default change.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = analyze(args.log)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('configurations', 'candidate_regular_cases',
                                           'candidate_failed_regular_cases', 'candidate_playback_accepted',
                                           'horizon_by_capacity')}, indent=2))
