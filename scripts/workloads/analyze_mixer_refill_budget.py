#!/usr/bin/env python3
"""Check the exact hosted post-discovery refill-delay matrix and its limits."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path


DELAYS = (0, 1, 9, 10, 19, 20, 21, 39, 40, 41, 50, 75)
GEOMETRY = {3: ([4416, 8192], [64, 2048]),
            4: ([5120, 8192], [128, 2048]),
            5: ([6016, 8192], [128, 2048]),
            6: ([8192, 5120], [2048, 128]),
            7: ([8192, 6016], [2048, 128])}


def analyze(path):
    text = path.read_text()
    if 'AUDIO_DELIVERY PASS' not in text or 'FAIL' in text:
        raise ValueError('Fixture integrity did not pass')
    rows, seen = [], set()
    for line in text.splitlines():
        if not line.startswith('MIXER_REFILL_BUDGET '):
            continue
        row = {}
        for field in line.split()[1:]:
            key, value = field.split('=')
            if key in row:
                raise ValueError('Duplicate metric')
            values = [int(x) for x in value.split(',')]
            row[key] = values if len(values) > 1 else values[0]
        identity = tuple(row[k] for k in ('shape', 'delay_ms', 'offset', 'reverse', 'scenario'))
        if identity in seen:
            raise ValueError('Duplicate configuration')
        seen.add(identity)
        shape, delay, _, _, scenario = identity
        if shape not in GEOMETRY:
            raise ValueError('Unknown geometry')
        capacity, fragment = GEOMETRY[shape]
        budget = 40 if shape in (5, 7) else 20
        if (row['capacities'] != capacity or row['fragments'] != fragment
                or row['budget_ms'] != budget or row['mode'] != 3 or row['refresh'] != 1
                or row['fragment'] != 1 or row['max_refill_delay_ms'] != [delay, 0]
                or not row['opportunities'][0] or row['interval_frames'] != 176400
                or row['drain_ms'] <= 6000 or row['completion'] != row['physical']
                or any(not 0 <= x <= 176400 for x in row['physical'])):
            raise ValueError('Unexpected geometry, delay or measurement interval')
        row['source_rates'] = [x / 176400 for x in row['physical']]
        row['strict_nominal'] = (row['physical'] == [176400, 176400]
                                 and row['steady_reservations'] == 0
                                 and row['missing_short'] == row['missing_empty'] == 0)
        rows.append(row)
    expected = set(itertools.product(range(3, 8), DELAYS, (0, 1, 4), (0, 1), (0,)))
    expected.update((shape, 40 if shape in (5, 7) else 20, 1, reverse, scenario)
                    for shape, reverse, scenario in itertools.product(range(3, 8), (0, 1), (1, 2, 3)))
    if seen != expected:
        raise ValueError('Incomplete or unexpected refill-budget matrix')
    within = [r for r in rows if r['shape'] != 3 and r['scenario'] == 0
              and r['delay_ms'] <= r['budget_ms']]
    if not all(r['strict_nominal'] for r in within):
        raise ValueError('Declared budget loses source data or misses nominal progress')
    inactive = [r for r in rows if r['shape'] != 3 and r['scenario'] in (1, 3)]
    if not all(r['physical'] == [176400, 0] and not r['steady_reservations'] for r in inactive):
        raise ValueError('Dormant/disabled peer harms the active supplier within budget')
    for row in rows:
        if row['scenario'] == 2:
            if row['recovery_service_resume_ms'] != 3041 or not 0 <= row['recovery_ms'] < 100:
                raise ValueError('Post-service recovery is unproven')
    groups = {}
    for shape, delay in itertools.product(range(3, 8), DELAYS):
        subset = [r for r in rows if r['shape'] == shape and r['delay_ms'] == delay and r['scenario'] == 0]
        groups[f'{shape}/{delay}'] = dict(cases=len(subset),
            strict_failures=sum(not r['strict_nominal'] for r in subset),
            min_source_rates=[min(r['source_rates'][s] for r in subset) for s in (0, 1)],
            missing_frames=sum(r['missing_short'] + r['missing_empty'] for r in subset),
            reservations=sum(r['steady_reservations'] for r in subset))
    return dict(configurations=len(rows), within_budget_cases=len(within),
                inactive_peer_cases=len(inactive), budget_contract_accepted=True,
                log_sha256=hashlib.sha256(path.read_bytes()).hexdigest(), groups=groups, rows=rows,
                scope='Instantaneous explicit completion discovery at1ms steps, followed by declared '
                      'producer0 admission delay; producer1 immediate. Fixed4096 horizon/12KiB HDA lead, '
                      '10ms service with0/1/4ms phases and both producer orders. Greedy whole-fragment '
                      'refill, not a native scheduler/codec model or arbitrary jitter guarantee. '
                      'Beyond-budget and interrupted-service failures retained; no default change.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    result = analyze(args.log)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: result[k] for k in ('configurations', 'within_budget_cases',
                                           'inactive_peer_cases', 'budget_contract_accepted')}, indent=2))
