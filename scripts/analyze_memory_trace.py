#!/usr/bin/env python3
"""Summarize completed Brook memory traces; counts are virtual requests, not RSS.

Requires atomic records with comm attribution. Legacy split records and error
suppression make the capture incomplete and cause CLI failure. Tracing starts
mid-process, so this tool deliberately does not infer complete live mappings.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re

MEMORY = 'mmap|munmap|mprotect|brk|mremap|msync|mincore|madvise'
PREFIX = re.compile(r'\[strace:(\d+)\] (' + MEMORY + r')\(')
RECORD = re.compile(
    r'\[strace:(\d+)\] (' + MEMORY + r')\(([^\n]*)\) = '
    r'(-?0x[0-9a-f]+|-?\d+)(?: \(err\))? comm="([^"\n]*)"\n')
SUPPRESSION = re.compile(r'\[strace:\d+\] .*suppress[^\n]*')


def analyze(trace):
    groups = {}
    matches = list(RECORD.finditer(trace))
    malformed = 0
    for match in matches:
        pid, name, arguments, result, comm = match.groups()
        try:
            args = [int(value.strip(), 0) for value in arguments.split(',')]
            ret = int(result, 0)
            if len(args) != (3 if name == 'brk' else 6):
                raise ValueError('unexpected argument count')
        except ValueError:
            malformed += 1
            continue
        key = (int(pid), comm)
        row = groups.setdefault(key, dict(pid=int(pid), comm=comm,
            calls=Counter(), errors=Counter(), mmap_requests=0,
            mmap_successes=0, mmap_requested_bytes=0,
            mmap_successful_requested_bytes=0, anonymous_sizes=Counter(),
            candidate_requests=0, candidate_successes=0,
            candidate_requested_bytes=0, candidate_successful_requested_bytes=0))
        row['calls'][name] += 1
        error = -4096 < ret < 0
        if error:
            row['errors'][name] += 1
        if name != 'mmap':
            continue
        _, size, prot, flags, _, _ = args
        row['mmap_requests'] += 1
        row['mmap_requested_bytes'] += size
        if not error:
            row['mmap_successes'] += 1
            row['mmap_successful_requested_bytes'] += size
        if flags & 0x20:
            row['anonymous_sizes'][str(size)] += 1
        # Diagnostic enrollment candidate only: private anonymous RW, no fixed
        # address/populate, within the explicit pager's current Region bound.
        if (flags & 3 == 2 and flags & 0x20 and prot == 3
                and not flags & (0x10 | 0x100000 | 0x8000)
                and 65536 <= size <= 16 * 1024 * 1024):
            row['candidate_requests'] += 1
            row['candidate_requested_bytes'] += size
            if not error:
                row['candidate_successes'] += 1
                row['candidate_successful_requested_bytes'] += size
    unmatched = len(PREFIX.findall(trace)) - len(matches)
    suppressed = SUPPRESSION.findall(trace)
    return dict(complete=bool(matches) and not (unmatched or malformed or suppressed),
                unmatched_records=unmatched, malformed_records=malformed,
                suppression_records=len(suppressed),
                scope='Observed completed virtual-memory calls only; not RSS, '
                      'unique bytes, lifetime peaks or untraced allocations. '
                      'Candidate counts do not establish safe enrollment.',
                processes=[groups[key] for key in sorted(groups)])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    report = analyze(args.trace.read_text(errors='replace'))
    rendered = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.write_text(rendered)
    else:
        print(rendered, end='')
    return 0 if report['complete'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
