#!/usr/bin/env python3
"""Analyze buffered browser samples independently of delayed HTTP delivery.

Requires the frozen phase receipts produced by the mixed workload controller.
Mapping page time to host time assumes the independently checked 1x KVM clock.
Two milliseconds of browser timestamp quantization are allowed per anchor.
RAF is a callback opportunity, not proof of physical presentation.
"""
import argparse
import json
from pathlib import Path


def analyze(root):
    def read(name):
        return json.loads((root / (name + '.json')).read_text())

    summary = read('summary')
    exported = read('buffered-trace')['state']
    epoch = exported['epoch']
    states = [row['state'] for row in read('all-states')
              if row['state']['epoch'] == epoch]
    if not states:
        return {'status': 'missing_clock_anchors'}
    tolerance = 2_000_000
    low = max(s['clock']['hostMonotonicNs'] -
              s['clock']['afterPerfMs'] * 1e6 - tolerance for s in states)
    high = min(s['clock']['hostMonotonicNs'] -
               s['clock']['beforePerfMs'] * 1e6 + tolerance for s in states)
    result = dict(status='ok' if low <= high else 'inconsistent_clock_anchors',
                  schema=2, phase=summary['phase'], epoch=epoch,
                  clock_anchors=len(states), offset_low_ns=low, offset_high_ns=high,
                  quantization_tolerance_ns=tolerance,
                  assumed_host_page_rate=1,
                  trace_dropped=exported['traceDropped'])
    if low > high:
        return result
    start, end = summary['load_host_start_latest_ns'], summary['load_host_end_earliest_ns']
    samples = exported['samples']
    loaded = [s for s in samples if start <= s['performanceMs'] * 1e6 + low
              and s['performanceMs'] * 1e6 + high <= end]
    result['total_samples'] = len(samples)
    result['loaded_samples'] = len(loaded)
    if len(loaded) >= 2:
        a, z = loaded[0], loaded[-1]
        elapsed = (z['performanceMs'] - a['performanceMs']) / 1000
        if elapsed <= 0:
            raise ValueError('Non-increasing page samples')
        result['loaded'] = dict(
            page_s=elapsed, media_s=z['mediaTotal'] - a['mediaTotal'],
            media_per_page_s=(z['mediaTotal'] - a['mediaTotal']) / elapsed,
            callbacks_per_page_s=(z['callbacks'] - a['callbacks']) / elapsed,
            total_video_frames_per_page_s=(z['totalFrames'] - a['totalFrames']) / elapsed,
            dropped_frames=z['droppedFrames'] - a['droppedFrames'],
            max_sample_gap_ms=max(y['performanceMs'] - x['performanceMs']
                                  for x, y in zip(loaded, loaded[1:])),
            buffered_below_half_second_samples=sum(s['bufferedAhead'] < .5 for s in loaded),
            min_buffered_ahead_s=min(s['bufferedAhead'] for s in loaded),
            not_playable_samples=sum(s['ready'] < 3 for s in loaded),
            ambiguous_media_gaps=z['ambiguousGaps'] - a['ambiguousGaps'])
    else:
        result['status'] = 'insufficient_loaded_samples'
    inputs = []
    for key in 'ABCD':
        request = read('key-' + key)
        values = [v for v in exported['inputs'] if v['key'] == 'Key' + key]
        if len(values) != 1:
            inputs.append(dict(key=key, status='missing_or_ambiguous'))
            continue
        value = values[0]
        handled = value['handledPerfMs'] * 1e6
        inputs.append(dict(key=key,
                           dispatch_to_dom_low_ms=max(0, handled + low - request['host_after_ns']) / 1e6,
                           dispatch_to_dom_high_ms=(handled + high - request['host_before_ns']) / 1e6,
                           dom_to_raf_ms=value['rafPerfMs'] - value['handledPerfMs']))
    result['inputs'] = inputs
    result['media_events'] = exported['events']
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase_directory', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = analyze(args.phase_directory)
    payload = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(payload)
    else:
        print(payload, end='')
    return 0 if result['status'] == 'ok' else 1


if __name__ == '__main__':
    raise SystemExit(main())
