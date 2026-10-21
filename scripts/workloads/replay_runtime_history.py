"""Replay bounded runtime history against a consumed-steal reference.

Owner rows reuse the last sampled clock pair. The first retained row is an
unverified seed; truncated tails cannot establish whole-run attribution.
Reject discontinuous/invalid baseline epochs rather than treating them as zero.
"""
import argparse
import json
from pathlib import Path

def replay(root, arm=None, runtime_at_arm=None):
    if not __debug__:
        raise RuntimeError('Replay requires Python assertions; do not use -O')
    load = lambda p: json.loads(p.read_text())
    stop = load(root / 'cleanup-history-off.json')
    runtime = load(root / 'runtime-final.json')
    assert not stop['enabled'] and (not stop['overflow'])
    assert not stop['baseline_lost'], 'Baseline loss needs a separately scoped replay'
    assert stop['capacity'] == 64
    assert 0 < stop['capture_cpus'] <= stop['online_cpus'] <= 64
    if arm is not None:
        assert runtime_at_arm is not None and arm['enabled'] and (arm['epoch'] == stop['epoch'])
        assert arm['capture_cpus'] == stop['capture_cpus'] and arm['online_cpus'] == stop['online_cpus']
    lifetime_first_match = False
    seen = set()
    cpus = []
    trigger = None
    trigger_previous = None
    global_rows = []
    for cpu in range(stop['capture_cpus']):
        h = load(root / f'cleanup-history-cpu-{cpu}.json')
        assert not h['enabled'] and (not h['overflow']) and (h['epoch'] == stop['epoch']) and (h['sequence'] == stop['sequence'])
        assert h['cpu'] == cpu and h['count'] == len(h['entries']) <= 64
        for key in ('epoch', 'sequence', 'triggered', 'trigger_cpu', 'trigger_sequence', 'trigger_sample', 'trigger_floor_ns', 'trigger_caught_up', 'catchup_sequence', 'baseline_lost', 'all_frozen', 'capacity', 'capture_cpus', 'online_cpus'):
            assert h[key] == stop[key], (cpu, key)
        assert 0 <= h['post_events'] <= 16
        assert h['frozen'] == (h['post_events'] == 16)
        if stop['all_frozen']:
            assert h['frozen']
        if not stop['triggered']:
            assert not h['frozen'] and (not h['post_events'])
        entries = h['entries']
        assert entries
        previous = None
        consumed = None
        debt = None
        episodes = []
        catchups = []
        owners = []
        clamps = []
        charged = idle = verified_charged = verified_idle = 0
        for e in entries:
            assert 0 < e['sequence'] <= stop['sequence'] and e['sequence'] not in seen
            seen.add(e['sequence'])
            global_rows.append(e)
            assert e['wall_ns'] >= e['steal_ns']
            assert e['charged_ns'] >= 0 and e['idle_ns'] >= 0
            if previous is not None:
                assert e['sequence'] > previous['sequence']
                assert e['floor_before_ns'] == previous['floor_after_ns']
                assert (e['pid_before'], e['incarnation_before']) == (previous['pid_after'], previous['incarnation_after'])
            assert e['baseline_before'] and e['baseline_after'], 'Baseline loss needs a separately scoped replay'
            if e['kind'] == 'owner':
                assert not e['valid'] and (not e['attempts']) and (e['floor_before_ns'] == e['floor_after_ns']) and (e['charged_ns'] == e['idle_ns'] == 0)
                if previous is not None:
                    assert (e['wall_ns'], e['steal_ns']) == (previous['wall_ns'], previous['steal_ns'])
                if debt and (e['pid_before'], e['incarnation_before']) != (e['pid_after'], e['incarnation_after']):
                    owners.append(e)
            else:
                assert e['valid'] and 1 <= e['attempts'] <= 8 and e['kind'] in ('charged', 'idle', 'clamp')
                if e['kind'] == 'idle':
                    assert e['pid_before'] == 0
                if e['kind'] == 'charged':
                    assert e['pid_before'] != 0
                assert (e['pid_before'], e['incarnation_before']) == (e['pid_after'], e['incarnation_after'])
                if previous is not None:
                    assert e['wall_ns'] >= previous['wall_ns'] and e['steal_ns'] >= previous['steal_ns']
                    elapsed = e['wall_ns'] - previous['wall_ns']
                    pending = e['steal_ns'] - consumed
                    assert pending >= 0
                    deducted = min(elapsed, pending)
                    credit = elapsed - deducted
                    consumed += deducted
                    assert e['floor_after_ns'] == e['wall_ns'] - consumed
                    assert e['charged_ns'] == (credit if e['pid_before'] else 0) and e['idle_ns'] == (0 if e['pid_before'] else credit)
                    now_debt = e['steal_ns'] - consumed
                    if now_debt and (not debt):
                        episodes.append(dict(sequence=e['sequence'], debt_ns=now_debt, pid=e['pid_before'], incarnation=e['incarnation_before']))
                    if debt and (not now_debt):
                        catchups.append(dict(sequence=e['sequence'], prior_debt_ns=debt, pid=e['pid_before'], incarnation=e['incarnation_before']))
                    debt = now_debt
                    verified_charged += e['charged_ns']
                    verified_idle += e['idle_ns']
                if e['kind'] == 'clamp':
                    assert e['wall_ns'] - e['steal_ns'] < e['floor_before_ns'] and e['floor_after_ns'] == e['floor_before_ns']
                    clamps.append(e)
                else:
                    assert e['floor_after_ns'] == e['wall_ns'] - e['steal_ns']
                charged += e['charged_ns']
                idle += e['idle_ns']
            if consumed is None:
                consumed = e['wall_ns'] - e['floor_after_ns']
                assert consumed >= 0
                debt = e['steal_ns'] - consumed
                assert debt >= 0
            if e['sequence'] == stop['trigger_sequence']:
                assert cpu == stop['trigger_cpu'] and e['kind'] == 'clamp' and (e['sample'] == stop['trigger_sample']) and (e['floor_before_ns'] == stop['trigger_floor_ns'])
                trigger = e
                trigger_previous = previous
            previous = e
        cpus.append(dict(cpu=cpu, rows=len(entries), prefix_overwritten=h['prefix_overwritten'], first_sequence=entries[0]['sequence'], last_sequence=entries[-1]['sequence'], seed=dict(sequence=entries[0]['sequence'], wall_ns=entries[0]['wall_ns'], floor_after_ns=entries[0]['floor_after_ns']), clamps=clamps, debt_episodes_after_seed=episodes, catchups_after_seed=catchups, owner_changes_with_debt=owners, final_retained_debt_ns=debt, recorded_charged_ns=charged, recorded_idle_ns=idle, verified_after_seed_charged_ns=verified_charged, verified_after_seed_idle_ns=verified_idle))
    assert stop['all_frozen'] == all(load(root / f'cleanup-history-cpu-{cpu}.json')['frozen'] for cpu in range(stop['capture_cpus']))
    last_sample = -1
    for e in sorted(global_rows, key=lambda row: row['sequence']):
        assert e['sample'] >= last_sample
        if e['kind'] != 'owner':
            assert e['sample'] > last_sample
        last_sample = e['sample']
    if stop['triggered']:
        assert trigger is not None and trigger_previous is not None
        first = runtime['first_clamp']
        field_pairs = [('sample', 'sample'), ('wall_ns', 'wall_ns'), ('steal_ns', 'steal_ns'), ('pid_before', 'pid'), ('incarnation_before', 'incarnation'), ('floor_before_ns', 'service_floor_ns')]
        lifetime_first_match = first['valid'] and first['cpu'] == stop['trigger_cpu'] and all((trigger[a] == first[b] for a, b in field_pairs))
        if runtime_at_arm is None or runtime_at_arm['clamped_service'] == 0:
            assert lifetime_first_match, 'Trigger does not match lifetime first clamp'
        if lifetime_first_match:
            assert first['previous_wall_ns'] == trigger_previous['wall_ns'] and first['previous_steal_ns'] == trigger_previous['steal_ns']
            assert first['deficit_ns'] == trigger['floor_before_ns'] - (trigger['wall_ns'] - trigger['steal_ns'])
        tail = load(root / f"cleanup-history-cpu-{stop['trigger_cpu']}.json")['entries']
        derived = next((e['sequence'] for e in tail if e['sequence'] > stop['trigger_sequence'] and e['kind'] != 'owner' and e['valid'] and (e['wall_ns'] - e['steal_ns'] >= stop['trigger_floor_ns'])), 0)
        assert stop['trigger_caught_up'] == bool(derived) and stop['catchup_sequence'] == derived
        if derived:
            assert any((e['sequence'] == derived for e in cpus[stop['trigger_cpu']]['catchups_after_seed']))
    report = dict(history_epoch=stop['epoch'], triggered=stop['triggered'], caught_up_within_retained_tail=stop['trigger_caught_up'], baseline_lost=stop['baseline_lost'], trigger=trigger, cpus=cpus, runtime_failures={k: runtime[k] for k in ('invalid_samples', 'backwards_wall', 'backwards_steal', 'clamped_service', 'stale_identity', 'saturated') if runtime[k]}, whole_run_debt=runtime['debt'], scope='Retained-tail reference replay from a recorded seed, not independent epoch baseline or KVM publication proof. Owner records reuse clock pairs. No whole-run counter equivalence from truncated tails. A matching positive trace does not clear failed runtime acceptance.')
    if arm is not None and (not runtime_at_arm['clamped_service']) and runtime['clamped_service']:
        assert stop['triggered'] and lifetime_first_match, 'History failed to preserve first clamp after zero-clamp arm'
    report.update(all_frozen=stop['all_frozen'], capture_cpus=stop['capture_cpus'], lifetime_first_clamp_matches=lifetime_first_match, arm_epoch_verified=arm is not None, lifetime_clamps_at_arm_snapshot=runtime_at_arm['clamped_service'] if runtime_at_arm is not None else None)
    return report

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path, help='mixed-prefix-control directory')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--arm-review', type=Path, help='review with first confirmed arm snapshots')
    args = parser.parse_args()
    arm = runtime_at_arm = None
    if args.arm_review is not None:
        arm = json.loads((args.arm_review / 'early-history-arm.json').read_text())['result']
        runtime_at_arm = json.loads((args.arm_review / 'early-runtime-after-arm.json').read_text())
    report = replay(args.root, arm, runtime_at_arm)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(triggered=report['triggered'], first_match=report['lifetime_first_clamp_matches'], all_frozen=report['all_frozen'], runtime_failures=report['runtime_failures'], rows=sum((c['rows'] for c in report['cpus']))), indent=2))
if __name__ == '__main__':
    main()
