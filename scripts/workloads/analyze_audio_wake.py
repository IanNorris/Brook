#!/usr/bin/env python3
"""Join bounded audio/ready/timer diagnostics without treating gaps as evidence.

Input is a review's mixed-prefix-control directory, after reservation/service
analysis. This deliberately does not infer causes from RIPs or steal deltas.
"""
import argparse
import json
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def deadline_ns(clock, tick):
    require(tick >= clock['origin_ms'], 'deadline predates tick origin')
    return clock['origin_ns'] + (tick - clock['origin_ms']) * 1_000_000


def join_cycle(clock, cycle, watch_events, timer_events):
    matches = []
    for index, event in enumerate(watch_events):
        if not event['has_deadline']:
            continue
        due = deadline_ns(clock, event['deadline_tick_ms'])
        # wakeupTick = floor(elapsed ms) + 10, sampled between setup clocks.
        if (cycle['before_sleep_ns'] + 9_000_000 <= due <=
                cycle['after_deadline_ns'] + 10_000_000 and
                cycle['after_deadline_ns'] <= event['ready_ns'] <=
                event['dispatch_ns'] <= cycle['wake_ns']):
            matches.append((index, event, due))
    row = {'cycle': cycle, 'watch_match_count': len(matches)}
    if len(matches) != 1:
        row['status'] = 'unmatched' if not matches else 'ambiguous'
        return row
    index, event, due = matches[0]
    row.update(status='matched', watch_index=index, watch=event, deadline_ns=due,
               deadline_to_ready_ns=event['ready_ns'] - due,
               ready_to_dispatch_ns=event['dispatch_ns'] - event['ready_ns'],
               dispatch_to_worker_ns=cycle['wake_ns'] - event['dispatch_ns'])
    require(row['ready_to_dispatch_ns'] == event['queue_ns'], 'queue mismatch')
    require(sum(row[k] for k in ('deadline_to_ready_ns', 'ready_to_dispatch_ns',
                                'dispatch_to_worker_ns')) == cycle['wake_ns'] - due,
            'wake decomposition does not conserve elapsed time')
    scans = [(i, e) for i, e in enumerate(timer_events)
             if e['before_scan_ns'] <= event['ready_ns'] <= e['after_scan_ns']]
    row['timer_scan_match_count'] = len(scans)
    if len(scans) == 1:
        i, timer = scans[0]
        row.update(timer_index=i, timer=timer,
                   deadline_to_timer_arrival_ns=timer['arrival_ns'] - due,
                   timer_arrival_to_ready_ns=event['ready_ns'] - timer['arrival_ns'],
                   deadline_inside_previous_arrival_gap=(
                       timer['previous_arrival_ns'] <= due <= timer['arrival_ns']))
    return row


def validate_service_join(cycles, joined):
    # Intermediate indices are meaningful only for these exact records.
    # A stale join with the same long-lived worker must never attach a
    # different reservation to a newly collected cycle.
    for reservation in joined['events']:
        saved = {r['index']: r['record'] for r in reservation['worker_records']}
        require(len(saved) == len(reservation['worker_records']),
                'duplicate saved worker index')
        for index, record in saved.items():
            require(0 <= index < len(cycles) and cycles[index] == record,
                    'stale service join worker record')
        for index in reservation['current_service_record_indices']:
            require(index in saved and cycles[index]['worked'] and
                    cycles[index]['wake_ns'] <= reservation['current']['before_copy_ns'] <=
                    reservation['current']['after_position_ns'] <= cycles[index]['end_ns'],
                    'stale service join reservation interval')


def analyze(root):
    def load(name):
        return json.loads((root / name).read_text())

    clock = load('clock-domain-before.json')
    end = load('clock-domain-after.json')
    for sample in (clock, end):
        require(sample['source'] == 'pvclock' and sample['pvclock_cpu_mask'] == 255,
                'requires stable pvclock on all eight test CPUs')
        require(not sample['rejected_samples'] and not sample['rate_adjustments'],
                'tick-domain fallback or rate adjustment')
    for key in ('origin_ns', 'origin_ms', 'timer_ticks_per_ms',
                'initial_timer_ticks_per_ms'):
        require(clock[key] == end[key], 'tick domain changed: ' + key)
    service_join = load('reservation-service-join.json')
    require(service_join['collection_complete'], 'incomplete service collection')
    phases = []
    selected = ('firefox-alone-before', 'both-prefix-on-c-quake-focused',
                'firefox-after-quake')
    for name in selected:
        folder = root / name
        def read(stem):
            return json.loads((folder / (stem + '.json')).read_text())
        service = read('service-after')
        service_reset = read('service-before')
        require(service['enabled'] and service_reset['enabled'] and
                service['epoch'] == service_reset['epoch'] and not service_reset['cycles'],
                'service control changed or not reset')
        require(not any(service[k] for k in ('invalid_clock', 'contended_records', 'saturated')),
                'invalid service records')
        require(service['slow_cycles'] == len(service['events']) + service['excluded_slow'] and
                len(service['events']) <= 16, 'service retention mismatch')
        ready, reset = read('watch-after'), read('watch-reset')
        watch = ready['watch']
        timer, timer_reset = read('timer-after'), read('timer-before')
        require(ready['enabled'] and reset['enabled'] and
                ready['reset_generation'] == reset['reset_generation'] and
                ready['policy'] == reset['policy'], 'ready control changed')
        require(not any(ready[k] for k in ('unavailable_clock_events', 'backwards_clock')),
                'invalid ready clock')
        identity = (service['worker_pid'], service['worker_incarnation'])
        for w in (watch, reset['watch']):
            require((w['pid'], w['incarnation']) == identity, 'worker identity changed')
            require(not any(w[k] for k in ('invalid', 'invalid_activity', 'saturated')),
                    'invalid watch records')
        require(not reset['watch']['samples'] and not reset['watch']['events'],
                'watch not reset')
        require(watch['slow'] == len(watch['events']) + watch['excluded'] and
                len(watch['events']) <= 16, 'watch retention mismatch')
        require(timer['enabled'] and timer_reset['enabled'] and
                timer['epoch'] == timer_reset['epoch'] and not timer_reset['arrivals'],
                'timer control changed or not reset')
        require(not any(timer[k] for k in ('invalid_clock', 'contended_entries',
                    'contended_records', 'missing_steal_entries', 'backwards_steal',
                    'steal_exceeds_gap', 'migrated', 'saturated')), 'invalid timer records')
        require(timer['slow'] == len(timer['events']) + timer['excluded'] and
                len(timer['events']) <= 16, 'timer retention mismatch')
        for event in watch['events']:
            require(0 < event['ready_ns'] <= event['dispatch_ns'] and
                    event['queue_ns'] == event['dispatch_ns'] - event['ready_ns'],
                    'invalid ready interval')
        for event in timer['events']:
            keys = ('previous_arrival_ns', 'arrival_ns', 'scheduler_ns',
                    'after_runtime_ns', 'before_scan_ns', 'after_scan_ns', 'after_bsp_ns')
            values = [event[k] for k in keys]
            require(values == sorted(values) and values[0] > 0, 'invalid timer interval')
            require(event['gap_ns'] == values[1] - values[0], 'timer gap mismatch')
        joined = next(p for p in service_join['rows'] if p['phase'] == name)
        require(identity == (joined['worker_pid'], joined['worker_incarnation']),
                'service join identity mismatch')
        cycles = sorted(service['events'], key=lambda e: e['before_sleep_ns'])
        for cycle in cycles:
            require(0 < cycle['before_sleep_ns'] <= cycle['after_deadline_ns'] <=
                    cycle['wake_ns'] <= cycle['end_ns'], 'invalid worker interval')
            require(cycle['service_ns'] == cycle['end_ns'] - cycle['wake_ns'] and
                    cycle['late_lower_ns'] == max(0, cycle['wake_ns'] -
                        cycle['after_deadline_ns'] - 10_000_000), 'worker duration mismatch')
        validate_service_join(cycles, joined)
        rows = [join_cycle(clock, cycle, watch['events'], timer['events']) for cycle in cycles]
        used = [r['watch_index'] for r in rows if r['status'] == 'matched']
        require(len(used) == len(set(used)), 'watch reused for different cycles')
        for index, row in enumerate(rows):
            row['reservations_in_current_service'] = [
                dict(observation=e['observation'], reserved_bytes=e['reserved_bytes'],
                     gap_ns=e['gap_ns'], within_audio_rate_window=e['within_audio_rate_window'])
                for e in joined['events'] if index in e['current_service_record_indices']]
        phases.append(dict(phase=name, worker_identity=identity, watch=watch,
                           timer_excluded=timer['excluded'], cycles=rows))
    return dict(clock_before=clock, clock_after=end, phases=phases,
                scope='Diagnostic only: all enqueue/dispatch clocks add observer cost. '
                'Largest slow records are not a timeline; unmatched/ambiguous records remain unknown. '
                'ready_ns follows enqueue, dispatch_ns precedes completed context switch. '
                'Deadline late_ms is sampled later than ready_ns. Timer scan containment is temporal '
                'association, not unique wake causality. IRQ gaps do not distinguish masked interrupts, '
                'missing delivery, pre-entry delay or host scheduling. Separate wall/steal samples '
                'are not an atomic host-delay decomposition. No performance/default acceptance.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = analyze(args.root)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    for phase in report['phases']:
        rows = phase['cycles']
        print(phase['phase'], 'cycles', len(rows), 'matched',
              sum(r['status'] == 'matched' for r in rows), 'timer scans',
              sum(r.get('timer_scan_match_count') == 1 for r in rows))


if __name__ == '__main__':
    main()
