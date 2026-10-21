#!/usr/bin/env python3
"""Analyze bounded QEMU PipeWire writes, not physical speaker or DMA timing."""
import argparse
import calendar
import datetime
import hashlib
import json
import math
from pathlib import Path
import re


def require(condition, message):
    if not condition:
        raise ValueError(message)


def distribution(values):
    values = sorted(values)
    if not values:
        return dict(count=0)
    return dict(count=len(values), min_ns=values[0], max_ns=values[-1],
                p50_ns=values[math.ceil(len(values)*.5)-1],
                p95_ns=values[math.ceil(len(values)*.95)-1])


def analyze(text, start_ns, end_ns):
    require(start_ns < end_ns, 'invalid common enabled interval')
    writes, delayed = [], []
    previous_stamp = None
    excluded = dict(prefix=0, suffix=0)
    for index, line in enumerate(text.splitlines()):
        match = re.fullmatch(r'(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d+Z) '
                             r'(pw_write|audio_timer_delayed) (.*)', line)
        if not match:
            require(not re.search(r'\b(pw_write|audio_timer_delayed)\b', line),
                    'unparsed audio trace line')
            continue
        dt = datetime.datetime.fromisoformat(match[1].replace('Z', '+00:00'))
        stamp = calendar.timegm(dt.utctimetuple())*1_000_000_000 + dt.microsecond*1000
        require(previous_stamp is None or stamp >= previous_stamp, 'audio trace clock regressed')
        previous_stamp = stamp
        if stamp < start_ns:
            excluded['prefix'] += 1
            continue
        if stamp >= end_ns:
            excluded['suffix'] += 1
            continue
        row = dict(line_index=index, wall_ns=stamp)
        if match[2] == 'audio_timer_delayed':
            fields = re.fullmatch(r'interval (\d+) ms', match[3])
            require(fields, 'malformed delayed timer event')
            row['reported_virtual_interval_ms'] = int(fields[1])
            delayed.append(row)
            continue
        fields = re.fullmatch(r'filled=(-?\d+) avail=(-?\d+) index=(\d+) len=(\d+)', match[3])
        require(fields, 'malformed PipeWire write event')
        filled, avail, write_index, requested = map(int, fields.groups())
        require(0 <= filled and 0 <= avail and filled+avail <= 1 << 22 and
                0 <= write_index < 1 << 32, 'invalid PipeWire ring state')
        row.update(filled_bytes=filled, available_bytes=avail, write_index=write_index,
                   requested_bytes=requested, accepted_bytes=min(requested, avail),
                   highwater_bytes=filled+avail)
        writes.append(row)
    require(len(writes) >= 2, 'insufficient PipeWire write records')
    require(len({r['highwater_bytes'] for r in writes}) == 1, 'PipeWire watermark changed')
    transitions = []
    for a, z in zip(writes, writes[1:]):
        require(z['write_index'] == (a['write_index']+a['accepted_bytes']) % (1 << 32),
                'PipeWire index discontinuity: missing writes or multiple/restarted voices')
        removed = a['filled_bytes']+a['accepted_bytes']-z['filled_bytes']
        require(removed >= 0, 'PipeWire fill grew without a recorded write')
        transitions.append(dict(previous_line=a['line_index'], next_line=z['line_index'],
                                before_wall_ns=a['wall_ns'], after_wall_ns=z['wall_ns'],
                                gap_ns=z['wall_ns']-a['wall_ns'], removed_bytes=removed))
    accepted_before_last = sum(r['accepted_bytes'] for r in writes[:-1])
    removed = sum(r['removed_bytes'] for r in transitions)
    require(accepted_before_last-removed == writes[-1]['filled_bytes']-writes[0]['filled_bytes'],
            'PipeWire queue conservation failed')
    positive = [r for r in writes if r['accepted_bytes']]
    gaps = [dict(before_wall_ns=a['wall_ns'], after_wall_ns=z['wall_ns'],
                 gap_ns=z['wall_ns']-a['wall_ns']) for a, z in zip(positive, positive[1:])]
    return dict(common_enabled_window=dict(start_wall_ns=start_ns, end_wall_ns=end_ns),
                excluded_transition_events=excluded, writes=writes, delayed_timers=delayed,
                highwater_bytes=writes[0]['highwater_bytes'],
                total_accepted_bytes=sum(r['accepted_bytes'] for r in writes),
                removed_between_observed_writes_bytes=removed,
                positive_write_gap=distribution([r['gap_ns'] for r in gaps]),
                longest_positive_write_gaps=sorted(gaps, key=lambda r:r['gap_ns'], reverse=True)[:16],
                largest_ring_removals=sorted(transitions, key=lambda r:r['removed_bytes'], reverse=True)[:16],
                scope='Pinned QEMU qpw_write trace precedes min(requested,available) ring publication. '
                      'Index continuity and queue conservation qualify a single continuous voice; '
                      'logger has no producer drop counter. Removed bytes left the PipeWire backend '
                      'ring, not necessarily the speakers. Positive write gaps are not PipeWire '
                      'callback periods or HDA transfer timestamps. Delayed-timer intervals use '
                      'QEMU virtual time and integer milliseconds, not host wall elapsed. Separate '
                      'post-timing diagnostic; no per-source tempo or uninterrupted-output acceptance.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path, help='post-timing-gpu-host-trace directory')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    folder = args.folder
    common = json.loads((folder/'analysis.json').read_text())
    raw = (folder/'qemu-fences.log').read_bytes()
    require(hashlib.sha256(raw).hexdigest() == common['trace_sha256'], 'trace changed since window validation')
    for name in ('before', 'enabled', 'disabled'):
        states = json.loads((folder/('event-states-'+name+'.json')).read_text())
        for event in ('pw_write', 'audio_timer_delayed'):
            require(any(r['name'] == event and r['state'] == ('enabled' if name == 'enabled' else 'disabled')
                        for r in states), 'audio trace event state mismatch')
    window = common['common_enabled_window']
    report = analyze(raw.decode(), window['start_wall_ns'], window['end_wall_ns'])
    report['trace_sha256'] = common['trace_sha256']
    report['capture_scope'] = json.loads((folder/'scope.json').read_text())
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(dict(writes=len(report['writes']), delayed_timers=len(report['delayed_timers']),
                          highwater_bytes=report['highwater_bytes'],
                          positive_write_gap=report['positive_write_gap'])))


if __name__ == '__main__':
    main()
