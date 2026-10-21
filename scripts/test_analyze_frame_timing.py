"""Reject incomplete/corrupt frame evidence before reporting percentiles."""
import unittest
from workloads.analyze_frame_timing import analyze, deadline_ns, PHASES


def fixture():
    def row(frame, start, previous, fence):
        phases = [0]*len(PHASES)
        phases[PHASES.index('other')] = 50_000
        phases[PHASES.index('preparation')] = 150_000
        phases[PHASES.index('fence_wait')] = 800_000
        return dict(frame=frame, begin_ns=start, end_ns=start+1_000_000,
                    previous_end_ns=previous, sleep_deadline_ticks=13+2*frame,
                    commands=3, command_bytes=200, fenced_commands=1,
                    first_fence=fence, last_fence=fence, phase_ns=phases)
    before = dict(applied=True, enabled=True, epoch=5, completed=0, retained=0,
                  contended_records=7)
    after = dict(applied=True, enabled=False, epoch=5, completed=2, retained=2,
                 overwritten=0, invalid_frames=0, clock_failures=0,
                 contended_records=7, aborted_frames=1)
    export = dict(after, capacity=256, export_consistent=True,
                  phase_names=list(PHASES), frames=[row(1, 10_000_000, 0, 42),
                                                  row(2, 12_000_000, 11_000_000, 43)])
    clock = dict(source='pvclock', origin_ms=5, origin_ns=2_000_000, rejected_samples=0)
    return dict(before=before, after=after, frames=export,
                clock_before=dict(clock), clock_after=dict(clock))


class FrameEvidenceTests(unittest.TestCase):
    def test_exclusive_accounting_and_sleep_are_separate(self):
        report = analyze(fixture())
        self.assertEqual(report['body']['p50_ns'], 1_000_000)
        self.assertEqual(report['start_to_start']['p50_ns'], 2_000_000)
        self.assertEqual(report['between_frame_gap']['p50_ns'], 1_000_000)
        self.assertEqual(report['installed_sleep_budget']['p50_ns'], 1_000_000)
        self.assertEqual(report['stages']['fence_wait']['share_of_body'], .8)
        self.assertEqual(sum(v['sum_ns'] for v in report['stages'].values()), 2_000_000)
        self.assertEqual(report['aborted_inflight_frames'], 1)

    def reject(self, change, message):
        data = fixture()
        change(data)
        with self.assertRaisesRegex(ValueError, message):
            analyze(data)

    def test_stage_double_counting_is_rejected(self):
        self.reject(lambda d: d['frames']['frames'][0]['phase_ns'].__setitem__(0, 6),
                    'conserve')

    def test_discontinuous_frame_identity_or_boundary_is_rejected(self):
        for field, value in [('frame', 3), ('previous_end_ns', 1099),
                             ('begin_ns', 1090), ('first_fence', 42)]:
            with self.subTest(field=field):
                self.reject(lambda d: d['frames']['frames'][1].__setitem__(field, value),
                            'Missing retained|Nonmonotone|Fence reused')

    def test_scope_loss_and_changed_export_are_rejected(self):
        for key, value in [('epoch', 6), ('completed', 3), ('export_consistent', False),
                           ('enabled', True), ('phase_names', ['total'])]:
            with self.subTest(key=key):
                self.reject(lambda d: d['frames'].__setitem__(key, value),
                            'Epoch|Export changed|consistent|schema')
        for field in ('invalid_frames', 'clock_failures', 'contended_records'):
            def change(data):
                for key in ('after', 'frames'):
                    data[key][field] += 1
            with self.subTest(field=field):
                self.reject(change, 'Invalid frame|Lost diagnostic')

    def test_overwritten_prefix_remains_visible(self):
        data = fixture()
        for key in ('after', 'frames'):
            data[key].update(completed=4, overwritten=2)
        for row in data['frames']['frames']:
            row['frame'] += 2
        report = analyze(data)
        self.assertEqual(report['overwritten_prefix'], 2)

    def test_missing_final_record_is_rejected(self):
        def change(data):
            for key in ('after', 'frames'):
                data[key].update(completed=3, overwritten=1)
        self.reject(change, 'Missing final')

    def test_early_dirty_wake_is_not_mislabeled_as_bad_clock(self):
        data = fixture()
        data['frames']['frames'][0]['sleep_deadline_ticks'] = 16
        report = analyze(data)
        self.assertEqual(report['early_resumes'], 1)
        self.assertEqual(report['resume_minus_deadline']['min_ns'], -1_000_000)

    def test_nonzero_tick_and_kvm_origins_use_checked_conversion(self):
        clock = fixture()['clock_before']
        self.assertEqual(deadline_ns(15, clock), 12_000_000)
        self.assertNotEqual(deadline_ns(15, clock), 15_000_000)
        for ticks, bad_clock in [(4, clock), ((1 << 64)-1, clock),
                                  (15, dict(clock, source='lapic-irqs'))]:
            with self.subTest(ticks=ticks, clock=bad_clock):
                with self.assertRaises(ValueError):
                    deadline_ns(ticks, bad_clock)
        self.reject(lambda d: d['clock_after'].__setitem__('origin_ns', 2_000_001),
                    'Clock origin')

    def test_parent_control_calls_reconcile_without_double_counting(self):
        data = fixture()
        for row in data['frames']['frames']:
            row['command_ns_by_parent'] = [0]*len(PHASES)
            row['commands_by_parent'] = [0]*len(PHASES)
            row['command_ns_by_parent'][PHASES.index('preparation')] = 800_000
            row['commands_by_parent'][PHASES.index('preparation')] = 3
        report = analyze(data)
        self.assertTrue(report['command_attribution_available'])
        parent = report['per_frame_control_call_elapsed_by_parent']['preparation']
        self.assertEqual(parent['sum_ns'], 1_600_000)
        self.assertEqual(parent['commands'], 6)
        self.assertEqual(report['body']['sum_ns'], 2_000_000)
        data['frames']['frames'][0]['command_ns_by_parent'][0] += 1
        with self.assertRaisesRegex(ValueError, 'attribution elapsed'):
            analyze(data)
        data['frames']['frames'][0]['command_ns_by_parent'][0] -= 1
        data['frames']['frames'][0]['commands_by_parent'][0] += 1
        with self.assertRaisesRegex(ValueError, 'attribution count'):
            analyze(data)
        del data['frames']['frames'][1]['command_ns_by_parent']
        with self.assertRaisesRegex(ValueError, 'attribution'):
            analyze(data)

    def test_unfenced_rows_do_not_invent_gpu_fence_latency(self):
        data = fixture()
        for row in data['frames']['frames']:
            row.update(fenced_commands=0, first_fence=0, last_fence=0)
        self.assertEqual(analyze(data)['rows'][0]['first_fence'], 0)
        data['frames']['frames'][0]['last_fence'] = 42
        with self.assertRaisesRegex(ValueError, 'Invalid fence'):
            analyze(data)


if __name__ == '__main__':
    unittest.main()
