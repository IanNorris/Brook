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
    keys = ('mode', 'capacity', 'fragment', 'refresh', 'offset', 'reverse', 'scenario')
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
    expected = set(itertools.product(range(3), (4096, 8192), range(2), range(2),
                                     (0, 1, 4), range(2), (0,)))
    expected.update(itertools.product(range(3), (4096,), (1,), (1,), (1,), range(2), (1, 2)))
    if seen != expected:
        raise ValueError('incomplete or unexpected experiment matrix')
    candidates = [row for row in rows if row['mode'] == 2 and row['scenario'] == 0]
    return dict(
        log_sha256=hashlib.sha256(log.read_bytes()).hexdigest(), configurations=len(rows),
        candidate_regular_cases=len(candidates),
        candidate_failed_regular_cases=sum(not row['regular_playback_gate'] for row in candidates),
        candidate_playback_accepted=all(row['regular_playback_gate'] for row in candidates),
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
                                           'candidate_failed_regular_cases', 'candidate_playback_accepted')}, indent=2))
