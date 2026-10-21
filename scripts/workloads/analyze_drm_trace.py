#!/usr/bin/env python3
"""Validate bounded libdrm aggregates; durations include host/scheduling waits."""
import argparse
import csv
import json
import re
from pathlib import Path


def analyze(text):
    lines = text.splitlines()
    match = re.fullmatch(
        r'# brook_drm_trace_v1 invalid_clock=(\d+) overflow=(\d+) saturated=(\d+) '
        r'incomplete=(\d+) first_ns=(\d+) last_ns=(\d+) skip_swaps=(\d+)',
        lines[0] if lines else '')
    if not match:
        raise ValueError('Invalid DRM trace header')
    invalid, overflow, saturated, incomplete, first, last, skip = map(int, match.groups())
    if any((invalid, overflow, saturated, incomplete)) or not 0 < first <= last or skip > 8192:
        raise ValueError('Incomplete, overflowed or invalid DRM trace')
    reader = csv.DictReader(lines[1:])
    if reader.fieldnames != ['tid', 'phase', 'request', 'calls', 'sum_ns', 'max_ns',
                            'errors', 'histogram_log2']:
        raise ValueError('Invalid DRM trace columns')
    rows, seen = [], set()
    for raw in reader:
        row = {key: int(raw[key], 0) if key == 'request' else int(raw[key])
               for key in reader.fieldnames[:-1]}
        hist = list(map(int, raw['histogram_log2'].split(';')))
        key = row['tid'], row['phase'], row['request']
        if (key in seen or not 0 < row['tid'] <= 0x3fffffff or
                row['phase'] not in (0, 1, 2) or not 0 <= row['request'] <= 0xffffffff or
                any(not 0 <= row[k] < 2**64 for k in ('calls', 'sum_ns', 'max_ns', 'errors')) or
                row['calls'] <= 0 or not 0 <= row['errors'] <= row['calls'] or
                not 0 <= row['max_ns'] <= row['sum_ns'] <= row['calls']*row['max_ns'] or
                row['max_ns'] > last-first or len(hist) != 64 or
                any(v < 0 for v in hist) or sum(hist) != row['calls']):
            raise ValueError('Invalid or inconsistent DRM aggregate')
        seen.add(key)
        lower = sum(n*(0 if i == 0 else 2**i) for i, n in enumerate(hist))
        upper = sum(n*(2**(i+1)-1) for i, n in enumerate(hist))
        if not lower <= row['sum_ns'] <= upper:
            raise ValueError('Histogram does not bound elapsed sum')
        last_bucket = max(i for i, n in enumerate(hist) if n)
        if last_bucket != max(0, row['max_ns'].bit_length()-1):
            raise ValueError('Histogram does not contain reported maximum')
        def quantile_bounds(numerator):
            target = (row['calls']*numerator+99)//100
            total = 0
            for i, count in enumerate(hist):
                total += count
                if total >= target:
                    return [0 if i == 0 else 2**i, min(2**(i+1)-1, row['max_ns'])]
        row.update(phase_name=['unscoped', 'between_swaps', 'inside_swap'][row['phase']],
                   mean_ns=row['sum_ns']/row['calls'],
                   p50_bucket_ns=quantile_bounds(50), p95_bucket_ns=quantile_bounds(95))
        rows.append(row)
    if not rows or len(rows) > 128:
        raise ValueError('Missing or oversized DRM aggregates')
    return dict(first_ns=first, last_ns=last, skip_swaps=skip, rows=rows,
                scope='Elapsed libdrm call time, including scheduling. Phase0 includes startup '
                      'and threads without SDL hooks. Inside/outside phases are per swap thread '
                      'after the configured skipped swaps; not exact demo marker bounds. '
                      'Concurrent thread times are not additive frame wall time. Percentiles '
                      'are histogram bucket bounds, not exact values.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.trace.read_text()), indent=2))
