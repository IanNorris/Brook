"""Reconcile stopped compositor stage rows before reporting elapsed times.

Input bundles acknowledged on/off replies and a stopped frames export. These
are guest elapsed intervals, including preemption/steal, not GPU execution.
"""
import argparse
import csv
import json
import math
from pathlib import Path

PHASES = ['other', 'deferred_captures', 'scene_drain', 'preparation',
          'texture_upload', 'quad_record', 'command_lock', 'vertex_upload',
          'command_build', 'queue_setup', 'fence_wait', 'ack_wait',
          'display_flush', 'debug_capture', 'debug_poll']


def require(condition, message):
    if not condition:
        raise ValueError(message)


def distribution(values):
    if not values:
        return {'count': 0}
    values = sorted(values)
    return dict(count=len(values), sum_ns=sum(values), min_ns=values[0],
                **{f'p{q}_ns': values[math.ceil(q*len(values)/100)-1]
                   for q in (50, 90, 95)}, max_ns=values[-1])


def deadline_ns(ticks, clock):
    require(clock['source'] == 'pvclock', 'Deadline mapping requires pvclock mode')
    maximum = (1 << 64)-1
    origin_ns, origin_ms = clock['origin_ns'], clock['origin_ms']
    require(all(isinstance(v, int) and 0 <= v <= maximum for v in
                (ticks, origin_ns, origin_ms)) and ticks >= origin_ms,
            'Invalid tick deadline or clock origin')
    ns = origin_ns + (ticks-origin_ms)*1_000_000
    require(ns <= maximum, 'Deadline conversion overflow')
    return ns


def analyze(bundle):
    before, after, export = (bundle[k] for k in ('before', 'after', 'frames'))
    require(before['applied'] and before['enabled'] and after['applied'] and
            not after['enabled'] and not export['enabled'] and export['export_consistent'],
            'Need acknowledged start, stop and consistent stopped export')
    require(before['epoch'] == after['epoch'] == export['epoch'], 'Epoch changed')
    require(export['phase_names'] == PHASES, 'Unsupported stage schema')
    clock_before, clock_after = (bundle[k] for k in ('clock_before', 'clock_after'))
    require(all(clock_before[k] == clock_after[k] for k in
                ('source', 'origin_ns', 'origin_ms', 'rejected_samples')),
            'Clock origin/mode changed or rejected samples occurred')
    for key in ('completed', 'retained', 'overwritten', 'invalid_frames',
                'clock_failures', 'contended_records', 'aborted_frames'):
        require(after[key] == export[key], f'Export changed: {key}')
    require(not export['invalid_frames'] and not export['clock_failures'],
            'Invalid frame or unavailable/backwards clock')
    require(export['contended_records'] == before['contended_records'],
            'Lost diagnostic records through contention')
    require(before['completed'] == before['retained'] == 0,
            'Capture must begin in a fresh empty epoch')
    rows = export['frames']
    require(len(rows) == export['retained'] and 0 < len(rows) <= export['capacity'] == 256,
            'Empty or inconsistent retained rows')
    require(export['overwritten'] == export['completed']-len(rows), 'Missing ring prefix')
    require(export['aborted_frames'] <= 1, 'Unexpected aborted frames')
    phase_values = {name: [] for name in PHASES}
    command_values = {name: [] for name in PHASES}
    command_counts = {name: 0 for name in PHASES}
    attribution = any('command_ns_by_parent' in r or 'commands_by_parent' in r for r in rows)
    body, periods, gaps, sleep_budget, deadline_lateness = [], [], [], [], []
    previous = None
    detailed = []
    for row in rows:
        deadline = deadline_ns(row['sleep_deadline_ticks'], clock_before)
        require(row['frame'] > 0 and 0 <= row['previous_end_ns'] <= row['begin_ns'] <= row['end_ns'],
                'Nonmonotone frame boundary')
        require(len(row['phase_ns']) == len(PHASES) and
                all(isinstance(v, int) and v >= 0 for v in row['phase_ns']), 'Invalid stages')
        duration = row['end_ns']-row['begin_ns']
        require(sum(row['phase_ns']) == duration, 'Stages do not conserve frame elapsed time')
        require(0 <= row['fenced_commands'] <= row['commands'] and row['command_bytes'] >= 0,
                'Invalid command counters')
        require((row['first_fence'] > 0 and row['last_fence'] >= row['first_fence'])
                if row['fenced_commands'] else row['first_fence'] == row['last_fence'] == 0,
                'Invalid fence identity')
        if attribution:
            require('command_ns_by_parent' in row and 'commands_by_parent' in row,
                    'Incomplete command attribution schema')
            durations, counts = row['command_ns_by_parent'], row['commands_by_parent']
            require(len(durations) == len(counts) == len(PHASES) and
                    all(isinstance(v, int) and v >= 0 for v in durations+counts),
                    'Invalid command attribution')
            require(sum(counts) == row['commands'], 'Command attribution count mismatch')
            nested = sum(row['phase_ns'][PHASES.index(k)] for k in
                         ('queue_setup', 'fence_wait', 'ack_wait'))
            overlap = row.get('command_preparation_ns', 0)
            require(isinstance(overlap, int) and
                    0 <= overlap <= row['phase_ns'][PHASES.index('command_build')],
                    'Invalid command preparation overlap')
            require(sum(durations) == nested + overlap, 'Command attribution elapsed mismatch')
            for name, ns, count in zip(PHASES, durations, counts):
                command_values[name].append(ns)
                command_counts[name] += count
        if previous is not None:
            require(row['frame'] == previous['frame']+1 and
                    row['previous_end_ns'] == previous['end_ns'], 'Missing retained frame')
            require(not row['first_fence'] or not previous['last_fence'] or
                    row['first_fence'] > previous['last_fence'], 'Fence reused across frames')
            periods.append(row['begin_ns']-previous['begin_ns'])
            gaps.append(row['begin_ns']-previous['end_ns'])
            previous_deadline = deadline_ns(previous['sleep_deadline_ticks'], clock_before)
            sleep_budget.append(max(0, previous_deadline-previous['end_ns']))
            deadline_lateness.append(row['begin_ns']-previous_deadline)
        body.append(duration)
        for name, ns in zip(PHASES, row['phase_ns']):
            phase_values[name].append(ns)
        detail = dict(frame=row['frame'], begin_ns=row['begin_ns'], end_ns=row['end_ns'],
                             body_ns=duration, **dict(zip(PHASES, row['phase_ns'])),
                             commands=row['commands'], control_request_bytes=row['command_bytes'],
                             fenced_commands=row['fenced_commands'], first_fence=row['first_fence'],
                             last_fence=row['last_fence'], sleep_deadline_ticks=row['sleep_deadline_ticks'],
                             sleep_deadline_kvm_ns=deadline)
        if 'command_preparation_ns' in row:
            detail['command_preparation_ns'] = row['command_preparation_ns']
        if attribution:
            detail.update({f'command_ns_{name}': ns for name, ns in zip(PHASES, durations)})
            detail.update({f'commands_{name}': count for name, count in zip(PHASES, counts)})
        detailed.append(detail)
        previous = row
    require(rows[-1]['frame'] == export['completed'], 'Missing final completed frame')
    total = sum(body)
    require(total > 0, 'No elapsed frame work')
    return dict(epoch=export['epoch'], retained=len(rows), overwritten_prefix=export['overwritten'],
                aborted_inflight_frames=export['aborted_frames'], body=distribution(body),
                start_to_start=distribution(periods), between_frame_gap=distribution(gaps),
                installed_sleep_budget=distribution(sleep_budget),
                resume_minus_deadline=distribution(deadline_lateness),
                early_resumes=sum(v < 0 for v in deadline_lateness),
                stages={name: dict(distribution(values), share_of_body=sum(values)/total)
                        for name, values in phase_values.items()}, rows=detailed,
                pending_command_preparation=distribution([r.get('command_preparation_ns', 0) for r in rows]),
                command_attribution_available=attribution,
                per_frame_control_call_elapsed_by_parent={
                    name: dict(distribution(values), commands=command_counts[name])
                    for name, values in command_values.items()} if attribution else None,
                scope='Exclusive guest elapsed stage totals conserve each recorded compositor thread cycle. '
                      'Includes preemption/host steal and instrumentation; other is explicit residual. '
                      'Sleep/wake gaps are separate, early dirty/input wakes are permitted. '
                      'Request bytes exclude transferred pixel/backing bytes. '
                      'Tick deadlines mapped with validated APIC/KVM origins; queue_setup includes response validation. '
                      'Control-call attribution includes queue_setup+waits+command_preparation_ns, not extra frame time. '
                      'command_preparation_ns is the subset of command_build while a request is logically outstanding; '
                      'it does not prove concurrent GPU execution. '
                      'No app FPS, physical scanout, GPU execution or scheduler latency attribution. '
                      'Do not sum distribution percentiles into a synthetic frame.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--csv', type=Path)
    args = parser.parse_args()
    report = analyze(json.loads(args.capture.read_text()))
    with args.output.open('x') as f:
        json.dump(report, f, indent=2)
        f.write('\n')
    if args.csv:
        with args.csv.open('x', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=list(report['rows'][0]))
            writer.writeheader()
            writer.writerows(report['rows'])
    print(json.dumps({k: report[k] for k in ('retained', 'body', 'start_to_start', 'stages')}, indent=2))


if __name__ == '__main__':
    main()
