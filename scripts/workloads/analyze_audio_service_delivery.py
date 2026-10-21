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


def mix_context(readings):
    """Optional enclosing counters, never a per-reservation attribution."""
    commands = [r['command'] for r in readings]
    if not any(c.startswith('audio contributions ') for c in commands):
        return None

    def one(command):
        rows = [r for r in readings if r['command'] == command]
        require(len(rows) == 1, 'missing or duplicate mix control: ' + command)
        return rows[0]['result']

    before, after = one('audio contributions reset'), one('audio contributions off')
    require(commands.index('audio contributions reset') < commands.index('audio delivery reset') <
            commands.index('audio delivery off') < commands.index('audio contributions off'),
            'contribution interval must contain delivery interval')
    require(before['enabled'] and not after['enabled'] and
            after['epoch'] == (before['epoch'] & ~1) + 2 and not before['observations'],
            'contribution generation changed or not reset')
    require(not any(after[k] for k in ('invalid', 'omitted_observations',
                'omitted_stream_frames', 'saturated', 'exhausted')), 'incomplete contribution accounting')
    require(after['count'] == len(after['streams']) <= after['capacity'], 'contribution row count mismatch')
    require(len({(r['identity'], r['epoch']) for r in after['streams']}) == len(after['streams']),
            'duplicate contribution identity')
    for row in after['streams']:
        missing = row['missing_empty_frames'] + row['missing_short_frames']
        require(row['accepted_output_frames'] == row['contributed_frames'] + missing,
                'contribution conservation mismatch')
        require(sum(row['missing_software_bins']) == sum(row['missing_inflight_bins']) == missing,
                'contribution histogram mismatch')
        require(row['accepted_output_frames'] <= after['output_frames'], 'stream exceeds unique output')
    horizon = [r for r in readings if r['command'] == 'audio horizon status']
    require(len(horizon) == 2, 'requires enclosing horizon snapshots')
    require(commands.index('audio contributions reset') < commands.index('audio horizon status') <
            commands.index('audio delivery reset'), 'horizon begin does not precede delivery reset')
    require(readings.index(horizon[1]) > commands.index('audio delivery off') and
            readings.index(horizon[1]) < commands.index('audio contributions off'),
            'horizon end does not enclose delivery stop')
    a, z = (r['result'] for r in horizon)
    for key in ('epoch', 'target_frames', 'refill_allowance_frames', 'refill_allowance_ms'):
        require(a[key] == z[key], 'horizon configuration changed: ' + key)
    for value in (a, z):
        require(value['enabled'] and not any(value[k] for k in ('saturated', 'exhausted',
                    'invalid_clock', 'unsupported_driver')), 'invalid horizon counters')
    delta = {}
    for key in ('decisions', 'deferrals', 'shortened_attempts', 'shortened_receipts',
                'accepted_prefix_frames', 'headroom_bypasses'):
        require(z[key] >= a[key], 'horizon counter regressed: ' + key)
        delta[key] = z[key] - a[key]
    return dict(contributions=after, horizon_before=a, horizon_after=z, horizon_delta=delta,
                scope='Counters enclose HDA recording but controls are sequential, not sample-aligned. '
                      'Accepted contribution conservation is not source demand or per-reservation cause. '
                      'Horizon decisions require positive source data, while zero-source attempts are '
                      'unrecorded here. Deferrals have no receipt. Do not subtract decisions from worker '
                      'cycles to infer exact empty attempts or attach aggregate loss to a particular gap.')


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
        # Previous publication end is unchanged before the next recorded
        # submission reserves its gap. lead_before uses raw completion;
        # pending_before_reserve uses completion rounded down to four bytes.
        # Therefore the position advance has at most three bytes uncertainty.
        # Previous position is read AFTER after_copy, current progress BEFORE
        # before_copy: these clocks give an upper bound on read-to-read time.
        raw_pending = p['lead_before_bytes'] + p['accepted_bytes'] - p['advanced_bytes']
        advance = raw_pending - c['pending_before_reserve']
        require(advance + 3 >= 0 and p['after_copy_ns'] <= lo, 'invalid position advance bracket')
        elapsed_upper = hi - p['after_copy_ns']
        position = dict(previous_raw_pending_bytes=raw_pending,
                        advance_bytes_bounds=[max(0, advance), advance + 3],
                        position_read_elapsed_upper_ns=elapsed_upper,
                        nominal_bytes_per_second=176400,
                        exceeds_nominal_upper=max(0, advance) * 1_000_000_000 > elapsed_upper * 176400,
                        scope='Fixed 44100-Hz stereo S16 HDA. Adjacent accepted observations, '
                              'same epoch and no omitted records; quantization bound, not physical '
                              'DAC timing. Compressed position advancement may reflect burst/catchup '
                              'or a clock discrepancy; not proof of sustained fast playback.')
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
                   previous_submission_gap_ns=hi-lo, worker_overlaps=overlaps,
                   position_advance=position)
        rows.append(row)
    return dict(worker_identity=identity, service=service, delivery=delivery, reservations=rows,
                fixed_lead_bytes=lead_bytes,
                mix_context=mix_context(readings),
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
