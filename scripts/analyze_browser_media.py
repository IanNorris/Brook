#!/usr/bin/env python3
"""Summarize browser media samples using source time, never HTTP arrival as FPS.

Server rows contain monotonic (host receipt) and state.{phase,performance_ms,t,
duration,frames,dropped}. Receipt order can differ from source order by seconds.
"""
import argparse
import json
import math
from pathlib import Path


def analyze(rows, phase, interval=None):
    rows = [r for r in rows if r['state'].get('phase') == phase and
            not r['state'].get('paused', True) and
            (interval is None or interval[0] <= r['monotonic'] <= interval[1])]
    if len(rows) < 2:
        raise ValueError('Need at least two playing samples for the requested phase/receipt interval')
    for row in rows:
        state = row['state']
        for key in ('performance_ms', 't', 'duration', 'frames', 'dropped'):
            value = state[key]
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
                raise ValueError('Invalid media sample: ' + key)
        if not 0 <= state['dropped'] <= state['frames'] or state['duration'] <= 0:
            raise ValueError('Invalid frame counts or duration')
    reordered = sum(b['state']['performance_ms'] < a['state']['performance_ms']
                    for a, b in zip(rows, rows[1:]))
    arrival_times = [r['monotonic'] for r in rows]
    rows = sorted(rows, key=lambda r: r['state']['performance_ms'])
    first, last = rows[0]['state'], rows[-1]['state']
    seconds = (last['performance_ms'] - first['performance_ms']) / 1000
    if seconds <= 0:
        raise ValueError('No source-time interval; cannot calculate rates')
    progress = 0
    loops = backwards = 0
    for a, b in zip(rows, rows[1:]):
        a, b = a['state'], b['state']
        if b['frames'] < a['frames'] or b['dropped'] < a['dropped']:
            raise ValueError('Media counter reset within one phase; split the observations')
        delta = b['t'] - a['t']
        if delta < 0 and a['t'] > .75 * a['duration'] and b['t'] < .25 * a['duration']:
            loops += 1
            delta += a['duration']
        elif delta < 0:
            backwards += 1
        progress += delta
    callback_rate = None
    if all(isinstance(r['state'].get('rvfc'), (int, float)) and
           not isinstance(r['state']['rvfc'], bool) and
           math.isfinite(r['state']['rvfc']) and r['state']['rvfc'] >= 0 for r in rows):
        if any(b['state']['rvfc'] < a['state']['rvfc'] for a, b in zip(rows, rows[1:])):
            raise ValueError('Video callback counter reset within one phase')
        callback_rate = (last['rvfc'] - first['rvfc']) / seconds
    return {'phase': phase, 'samples': len(rows), 'source_span_seconds': seconds,
            'receipt_span_seconds': max(arrival_times) - min(arrival_times),
            'out_of_order_receipts': reordered, 'media_loops': loops,
            'media_backsteps': backwards, 'media_progress_seconds': progress,
            'media_seconds_per_page_second': progress / seconds,
            'total_video_frames_per_page_second': (last['frames'] - first['frames']) / seconds,
            'dropped_frames': last['dropped'] - first['dropped'],
            'video_callbacks_per_page_second': callback_rate,
            'reported_presented_frames_per_page_second':
                ((last['frames'] - last['dropped']) -
                 (first['frames'] - first['dropped'])) / seconds,
            'scope': 'Rates use browser performance.now, not host time. HTTP receipt filtering does not align source samples to a PCM capture. Presented counts are browser-reported, not independently observed screen refreshes.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('states', type=Path)
    parser.add_argument('--phase', required=True)
    parser.add_argument('--capture', type=Path, help='Optional PCM metadata: filter by host receipt interval only')
    args = parser.parse_args()
    interval = json.loads(args.capture.read_text())['host_monotonic_interval'] if args.capture else None
    if interval is not None and (len(interval) != 2 or interval[1] <= interval[0]):
        parser.error('Invalid host capture interval')
    rows = [json.loads(line) for line in args.states.read_text().splitlines() if line.strip()]
    print(json.dumps(analyze(rows, args.phase, interval), indent=2))


if __name__ == '__main__':
    main()
