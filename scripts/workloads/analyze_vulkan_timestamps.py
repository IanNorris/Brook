#!/usr/bin/env python3
"""Validate optional correctness-probe timestamps; not a presentation benchmark."""
import argparse
import json
import math


def analyze(report):
    if report.get('passed') is not True:
        raise ValueError('correctness probe failed')
    timing = report['timing']
    bits, period = timing['valid_bits'], timing['period_ns']
    if not isinstance(bits, int) or not 1 <= bits <= 64:
        raise ValueError('invalid timestamp width')
    if not math.isfinite(period) or period <= 0:
        raise ValueError('invalid timestamp period')
    mask = (1 << bits) - 1

    def delta(a, b):
        if any(not isinstance(v, int) or not 0 <= v < 1 << 64 for v in (a, b)):
            raise ValueError('invalid raw timestamp')
        value = (b - a) & mask
        if value >= 1 << (bits - 1):
            raise ValueError('ambiguous wrap or reversed timestamps')
        return value * period

    calibrations = timing['calibrations']
    if len(calibrations) not in (0, 2):
        raise ValueError('incomplete calibration')
    for c in calibrations:
        if any(not isinstance(c[k], int) or c[k] < 0 for k in ('gpu_ticks', 'cpu_ns', 'max_deviation_ns')):
            raise ValueError('invalid calibration')
    if calibrations:
        delta(calibrations[0]['gpu_ticks'], calibrations[1]['gpu_ticks'])
        if calibrations[1]['cpu_ns'] < calibrations[0]['cpu_ns']:
            raise ValueError('CPU calibration clock reversed')
    rounds = []
    if len(timing['rounds']) != report['rounds'] or len(timing['rounds']) != 3:
        raise ValueError('missing completed rounds')
    for row in timing['rounds']:
        ticks, cpu = row['ticks'], row['cpu_ns']
        if len(ticks) != 4 or len(cpu) != 3 or any(not isinstance(v, int) or v < 0 for v in cpu) or cpu != sorted(cpu):
            raise ValueError('invalid round')
        segments = [delta(a, b) for a, b in zip(ticks, ticks[1:])]
        entry = dict(gpu_interval_ns=delta(ticks[0], ticks[-1]),
                     gpu_segments_ns=segments, submit_call_ns=cpu[1] - cpu[0],
                     submit_to_completion_return_ns=cpu[2] - cpu[0])
        if calibrations:
            # Retain both anchor estimates and their advertised uncertainty;
            # no claim that either is the host clock or a presentation time.
            estimates = []
            for c in calibrations:
                def mapped(t):
                    raw = (t - c['gpu_ticks']) & mask
                    signed = raw if raw < 1 << (bits - 1) else raw - (1 << bits)
                    return c['cpu_ns'] + signed * period
                estimates.append(dict(submit_to_gpu_start_ns=mapped(ticks[0]) - cpu[0],
                                      gpu_end_to_completion_return_ns=cpu[2] - mapped(ticks[-1]),
                                      max_deviation_ns=c['max_deviation_ns']))
            entry['calibrated_estimates'] = estimates
        rounds.append(entry)
    return dict(rounds=rounds, calibrated=bool(calibrations),
                scope='Diagnostic transfer/compute/readback intervals, including GPU stalls and instrumentation dependencies; no WSI, exclusive stage cost, host clock or physical scanout measurement.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report')
    args = parser.parse_args()
    with open(args.report) as stream:
        print(json.dumps(analyze(json.load(stream)), indent=2))
