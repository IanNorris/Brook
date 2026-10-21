#!/usr/bin/env python3
"""Join existing worker and HDA diagnostic intervals; missing cycles stay unknown.

Input is the persistent RPC guest-readings.json from a separate diagnostic
window. This does not infer audio source availability or scheduler ready time.
"""
import argparse
import json
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def analyze(readings, lead_bytes):
    require(lead_bytes > 0 and lead_bytes % 4 == 0, 'invalid fixed HDA lead')

    def one(command):
        rows = [r for r in readings if r['command'] == command]
        require(len(rows) == 1, 'missing or duplicate control: ' + command)
        return rows[0]['result']

    service_reset, service = one('audio timing reset'), one('audio timing off')
    delivery_reset, delivery = one('audio delivery reset'), one('audio delivery off')
    commands = [r['command'] for r in readings]
    require(commands.index('audio timing reset') < commands.index('audio delivery reset') <
            commands.index('audio delivery off') < commands.index('audio timing off'),
            'worker recording must contain delivery recording')
    for reset, end in ((service_reset, service), (delivery_reset, delivery)):
        # Low bit is enabled state; disabling advances the generation and
        # clears that bit (e.g. enabled epoch 5 becomes disabled epoch 6).
        require(reset['enabled'] and not end['enabled'] and
                end['epoch'] == (reset['epoch'] & ~1) + 2,
                'unexpected probe epoch or control state')
        require(not any(end[k] for k in ('invalid_clock', 'contended_records', 'saturated')),
                'invalid or omitted diagnostic observations')
    require(not service_reset['cycles'] and not delivery_reset['observations'], 'probe not reset')
    require(not delivery['invalid_position'] and not delivery['unsafe'], 'invalid HDA publication')
    identity = (service['worker_pid'], service['worker_incarnation'])
    require(identity[0] and identity == (service_reset['worker_pid'], service_reset['worker_incarnation']),
            'worker identity changed')
    cycles = sorted(service['events'], key=lambda e: e['before_sleep_ns'])
    require(len(cycles) <= 16 and service['slow_cycles'] == len(cycles) + service['excluded_slow'],
            'worker retention mismatch')
    require(service['cycles'] >= service['slow_cycles'], 'slow cycles exceed completed cycles')
    previous_end = 0
    for e in cycles:
        require(previous_end <= e['before_sleep_ns'] <= e['after_deadline_ns'] <=
                e['wake_ns'] <= e['end_ns'], 'invalid worker interval ordering')
        require(e['service_ns'] == e['end_ns'] - e['wake_ns'] and
                e['late_lower_ns'] == max(0, e['wake_ns'] - e['after_deadline_ns'] - 10_000_000),
                'worker duration mismatch')
        require(e['score_ns'] == max(e[k] for k in ('setup_ns', 'late_lower_ns', 'service_ns', 'tail_ns'))
                and e['score_ns'] >= 20_000_000, 'invalid slow-cycle score')
        previous_end = e['end_ns']
    pairs = delivery['reservation_events']
    require(len(pairs) == delivery['reservations_retained'] and
            len(pairs) + delivery['reservations_overwritten'] == delivery['reservations'],
            'reservation retention mismatch')
    require(len({p['observation'] for p in pairs}) == len(pairs), 'duplicate reservation observation')
    if not delivery['reservations_overwritten']:
        require(sum(p['current']['reserved_bytes'] for p in pairs) == delivery['reserved_bytes'],
                'reservation byte total mismatch')
    rows = []
    for pair in pairs:
        c, p = pair['current'], pair['previous']
        require(pair['control_epoch'] == delivery_reset['epoch'], 'stale reservation epoch')
        require(c['running'] and c['position_valid'] and c['publish_safe'] and
                c['reserved_bytes'] == max(0, lead_bytes - c['pending_before_reserve']),
                'reservation does not match fixed lead deficit')
        require(0 < c['before_copy_ns'] <= c['after_copy_ns'] <= c['after_position_ns'] and
                c['accepted_bytes'] > 0 and c['accepted_bytes'] % 4 == 0,
                'invalid reservation publication interval')
        row = dict(observation=pair['observation'], reserved_bytes=c['reserved_bytes'], current=c,
                   previous=p, has_previous=pair['has_previous'])
        if not pair['has_previous']:
            row['status'] = 'no preceding observed submission'
            rows.append(row)
            continue
        require(p and p['hardware_epoch'] == c['hardware_epoch'] and p['publish_safe'] and
                p['position_valid'] and p['running'], 'unqualified preceding submission')
        lo, hi = p['after_position_ns'], c['before_copy_ns']
        require(0 < lo <= hi <= c['after_position_ns'], 'invalid delivery interval')
        overlaps = []
        for index, e in enumerate(cycles):
            pieces = dict(sleep=(e['after_deadline_ns'], e['wake_ns']),
                          late_sleep=(e['after_deadline_ns'] + 10_000_000, e['wake_ns']),
                          service=(e['wake_ns'], e['end_ns']))
            overlap = {k: max(0, min(hi, z) - max(lo, a)) for k, (a, z) in pieces.items()}
            current_service = e['worked'] and e['wake_ns'] <= c['before_copy_ns'] <= c['after_position_ns'] <= e['end_ns']
            if any(overlap.values()) or current_service:
                overlaps.append(dict(index=index, cycle=e, positive_overlap_ns=overlap,
                                     contains_current_copy=bool(current_service)))
        row.update(status='qualified interval; unrecorded worker cycles remain unknown',
                   previous_submission_gap_ns=hi-lo, worker_overlaps=overlaps)
        rows.append(row)
    return dict(worker_identity=identity, service=service, delivery=delivery, reservations=rows,
                fixed_lead_bytes=lead_bytes,
                scope='Separate diagnostic, not performance acceptance. Worker top16 slow episodes '
                      'are not a timeline. Positive interval overlap is not unique cause. Missing '
                      'records cannot prove timely wake/service, sufficient source data or uninterrupted '
                      'PCM. Submission gaps alone do not establish missed worker deadlines. '
                      'No scheduler-ready, source-tempo or physical-speaker conclusion.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('readings', type=Path)
    parser.add_argument('--lead-bytes', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = analyze(json.loads(args.readings.read_text()), args.lead_bytes)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(cycles=report['service']['cycles'],
                          retained_slow=len(report['service']['events']),
                          excluded_slow=report['service']['excluded_slow'],
                          reservations=report['delivery']['reservations'],
                          overwritten=report['delivery']['reservations_overwritten'])))


if __name__ == '__main__':
    main()
