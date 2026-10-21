"""Adversarial checks for bounded scheduler accounting evidence.

The hand-calculated example crosses an owner change with 240 ns of pending
steal, then repays it over two samples. It intentionally has a partial tail.
No VM artifacts or licensed application data are needed.
"""
import json
from pathlib import Path
import tempfile
import unittest

from workloads.replay_runtime_history import replay


def fixture():
    def row(sequence, sample, kind, wall, steal, before, after,
            pid=7, next_pid=None, charged=0, idle=0):
        return dict(sequence=sequence, sample=sample, kind=kind, wall_ns=wall,
                    steal_ns=steal, floor_before_ns=before, floor_after_ns=after,
                    pid_before=pid, pid_after=pid if next_pid is None else next_pid,
                    incarnation_before=pid,
                    incarnation_after=pid if next_pid is None else next_pid,
                    charged_ns=charged, idle_ns=idle,
                    attempts=0 if kind == 'owner' else 1,
                    valid=kind != 'owner', baseline_before=True, baseline_after=True)

    # Seed credit is recorded, but cannot be independently checked without its
    # predecessor. Subsequent credits are 200, 0, 0, 0, 60, and 0 ns.
    rows = [row(1, 1, 'charged', 1000, 100, 890, 900, charged=10),
            row(2, 2, 'charged', 1200, 100, 900, 1100, charged=200),
            row(3, 3, 'clamp', 1210, 350, 1100, 1100),
            row(4, 3, 'owner', 1210, 350, 1100, 1100, next_pid=8),
            row(5, 4, 'clamp', 1310, 350, 1100, 1100, pid=8),
            row(6, 5, 'charged', 1510, 350, 1100, 1160, pid=8, charged=60),
            row(7, 5, 'owner', 1510, 350, 1160, 1160, pid=8, next_pid=0),
            row(8, 6, 'idle', 1610, 350, 1160, 1260, pid=0, idle=100)]
    stop = dict(enabled=False, overflow=False, baseline_lost=False,
                capture_cpus=1, online_cpus=1, capacity=64, epoch=9,
                sequence=8, triggered=True, trigger_cpu=0, trigger_sequence=3,
                trigger_sample=3, trigger_floor_ns=1100,
                trigger_caught_up=True, catchup_sequence=6, all_frozen=False)
    cpu = dict(stop, cpu=0, count=len(rows), entries=rows, frozen=False,
               post_events=5, prefix_overwritten=42)
    runtime = dict(invalid_samples=0, backwards_wall=0, backwards_steal=0,
                   clamped_service=2, stale_identity=0, saturated=False,
                   first_clamp=dict(valid=True, cpu=0, sample=3, wall_ns=1210,
                       steal_ns=350, pid=7, incarnation=7, service_floor_ns=1100,
                       previous_wall_ns=1200, previous_steal_ns=100, deficit_ns=240),
                   debt=dict(known_ns=0, episodes=1, catchups=1, owner_transitions=1))
    return {'cleanup-history-off.json': stop, 'cleanup-history-cpu-0.json': cpu,
            'runtime-final.json': runtime}


class ReplayTests(unittest.TestCase):
    def run_replay(self, data, arm=None, runtime_at_arm=None):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, value in data.items():
                (root / name).write_text(json.dumps(value))
            return replay(root, arm, runtime_at_arm)

    def reject_change(self, change):
        data = fixture()
        change(data)
        with self.assertRaises(AssertionError):
            self.run_replay(data)

    def test_debt_crosses_owner_and_repays_without_duplicate_credit(self):
        report = self.run_replay(fixture())
        cpu = report['cpus'][0]
        self.assertEqual(cpu['debt_episodes_after_seed'][0]['debt_ns'], 240)
        self.assertEqual(cpu['owner_changes_with_debt'][0]['pid_after'], 8)
        self.assertEqual(cpu['catchups_after_seed'][0]['prior_debt_ns'], 140)
        self.assertEqual(cpu['verified_after_seed_charged_ns'], 260)
        self.assertEqual(cpu['verified_after_seed_idle_ns'], 100)
        self.assertEqual(cpu['final_retained_debt_ns'], 0)
        self.assertFalse(report['all_frozen'])
        self.assertTrue(report['lifetime_first_clamp_matches'])
        # A successful reference replay keeps the failed native gate visible.
        self.assertEqual(report['runtime_failures'], {'clamped_service': 2})

    def test_seed_credit_is_explicitly_unverified(self):
        data = fixture()
        data['cleanup-history-cpu-0.json']['entries'][0]['charged_ns'] = 123456
        report = self.run_replay(data)
        cpu = report['cpus'][0]
        self.assertEqual(cpu['recorded_charged_ns'], 123716)
        self.assertEqual(cpu['verified_after_seed_charged_ns'], 260)
        self.assertIn('recorded seed', report['scope'])

    def test_corrupt_charge_owner_clock_and_floor_are_rejected(self):
        for index, field, value in [(1, 'charged_ns', 201), (3, 'charged_ns', 1),
                                     (3, 'wall_ns', 1211), (4, 'pid_before', 7),
                                     (4, 'floor_after_ns', 1110),
                                     (5, 'steal_ns', 349), (5, 'wall_ns', 1309),
                                     (5, 'idle_ns', 1), (5, 'attempts', 0),
                                     (5, 'baseline_before', False)]:
            with self.subTest(index=index, field=field):
                self.reject_change(lambda d: d['cleanup-history-cpu-0.json']
                                   ['entries'][index].__setitem__(field, value))

    def test_epoch_order_freeze_and_trigger_corruption_are_rejected(self):
        for field, value in [('epoch', 10), ('sequence', 9), ('frozen', True),
                             ('post_events', 17), ('trigger_sample', 4)]:
            with self.subTest(field=field):
                self.reject_change(lambda d: d['cleanup-history-cpu-0.json']
                                   .__setitem__(field, value))
        self.reject_change(lambda d: d['cleanup-history-cpu-0.json']['entries'][4]
                           .__setitem__('sequence', 4))

    def test_mismatched_lifetime_previous_pair_is_rejected(self):
        self.reject_change(lambda d: d['runtime-final.json']['first_clamp']
                           .__setitem__('previous_steal_ns', 101))

    def test_lost_baseline_or_false_catchup_is_rejected(self):
        for field, value in [('baseline_lost', True), ('catchup_sequence', 5),
                             ('trigger_caught_up', False), ('all_frozen', True)]:
            with self.subTest(field=field):
                def change(data):
                    for name in ('cleanup-history-off.json', 'cleanup-history-cpu-0.json'):
                        data[name][field] = value
                self.reject_change(change)

    def test_confirmed_arm_epoch_is_checked(self):
        arm = dict(fixture()['cleanup-history-off.json'], enabled=True)
        snapshot = dict(clamped_service=0)
        self.assertTrue(self.run_replay(fixture(), arm, snapshot)['arm_epoch_verified'])
        arm['epoch'] += 1
        with self.assertRaises(AssertionError):
            self.run_replay(fixture(), arm, snapshot)

    def test_preexisting_lifetime_clamp_does_not_claim_first_match(self):
        data = fixture()
        data['runtime-final.json']['first_clamp']['sample'] = 1
        arm = dict(data['cleanup-history-off.json'], enabled=True)
        report = self.run_replay(data, arm, dict(clamped_service=1))
        self.assertFalse(report['lifetime_first_clamp_matches'])
        with self.assertRaises(AssertionError):
            self.run_replay(data, arm, dict(clamped_service=0))


if __name__ == '__main__':
    unittest.main()
