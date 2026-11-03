#!/usr/bin/env python3
"""Summarize one process's descriptor classes without reporting paths.

Run on the guest. Repeated samples are observations, not an atomic trace; a
process may exit, recycle descriptors, or be replaced between operations.
"""
import argparse
from collections import Counter
import json
import math
import os
from pathlib import Path
import time


def snapshot(pid, proc=Path('/proc')):
    root = proc / str(pid)
    status = (root / 'status').read_text()
    fields = dict(line.split(':', 1) for line in status.splitlines() if ':' in line)
    if fields.get('Pid', '').strip() != str(pid):
        raise RuntimeError('process identity mismatch')
    # Do not return link targets: they may contain account names or other
    # sensitive paths. A failed read is distinct from an empty table.
    descriptors = list((root / 'fd').iterdir())
    counts = Counter()
    missing = 0
    for entry in descriptors:
        if not entry.name.isdecimal():
            continue
        try:
            target = os.readlink(entry)
        except OSError:
            missing += 1
            continue
        kind = target if target in ('memfd', 'unix', 'epoll', 'eventfd', 'pipe', 'socket') else 'other'
        counts[kind] += 1
    return {'pid': pid, 'name': fields.get('Name', '').strip(),
            'tgid': fields.get('Tgid', '').strip(),
            'fd_entries': len(descriptors), 'unreadable_or_raced': missing,
            'classes': dict(sorted(counts.items()))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid', type=int)
    parser.add_argument('--samples', type=int, default=1)
    parser.add_argument('--interval', type=float, default=1.0)
    args = parser.parse_args()
    if args.pid <= 0 or not 1 <= args.samples <= 60 or not math.isfinite(args.interval) or not 0 <= args.interval <= 60:
        parser.error('pid must be positive; samples 1–60; interval 0–60 seconds')
    for i in range(args.samples):
        try:
            result = snapshot(args.pid)
            result.update(sample=i, clock_monotonic=time.monotonic())
        except (OSError, RuntimeError) as exc:
            print(json.dumps({'sample': i, 'pid': args.pid, 'error': type(exc).__name__}), flush=True)
            return 1
        print(json.dumps(result), flush=True)
        if i + 1 < args.samples:
            time.sleep(args.interval)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
