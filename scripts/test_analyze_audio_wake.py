#!/usr/bin/env python3
"""Check joins against known elapsed segments, including missing/ambiguous data."""
import copy
import unittest
from workloads.analyze_audio_wake import deadline_ns, join_cycle, validate_service_join


class WakeJoinTests(unittest.TestCase):
    def setUp(self):
        # Nonzero, non-ms-aligned origin catches naive tick*1e6 conversion.
        self.clock = dict(origin_ns=2_302_485_800, origin_ms=754)
        self.due = deadline_ns(self.clock, 140481)
        self.cycle = dict(before_sleep_ns=self.due - 9_500_000,
                          after_deadline_ns=self.due - 9_499_900,
                          wake_ns=self.due + 91_130_388,
                          end_ns=self.due + 91_244_380)
        self.event = dict(has_deadline=True, deadline_tick_ms=140481,
                          ready_ns=self.due + 91_110_844,
                          dispatch_ns=self.due + 91_124_791, queue_ns=13947)
        self.timer = dict(previous_arrival_ns=self.due - 850395,
                          arrival_ns=self.due + 91_086_035,
                          before_scan_ns=self.due + 91_088_258,
                          after_scan_ns=self.due + 91_180_288)

    def join(self, events=None, timers=None):
        return join_cycle(self.clock, self.cycle,
                          [self.event] if events is None else events,
                          [self.timer] if timers is None else timers)

    def test_known_segments_and_origin(self):
        self.assertEqual(self.due, 142_029_485_800)
        row = self.join()
        self.assertEqual(row['status'], 'matched')
        self.assertEqual(row['deadline_to_ready_ns'], 91_110_844)
        self.assertEqual(row['ready_to_dispatch_ns'], 13947)
        self.assertEqual(row['dispatch_to_worker_ns'], 5597)
        self.assertEqual(row['timer_arrival_to_ready_ns'], 24809)
        self.assertTrue(row['deadline_inside_previous_arrival_gap'])

    def test_missing_data_is_unknown(self):
        self.assertEqual(self.join(events=[])['status'], 'unmatched')
        row = self.join(timers=[])
        self.assertEqual(row['status'], 'matched')
        self.assertEqual(row['timer_scan_match_count'], 0)
        self.assertNotIn('deadline_to_timer_arrival_ns', row)

    def test_duplicate_watch_is_ambiguous(self):
        row = self.join(events=[self.event, copy.deepcopy(self.event)])
        self.assertEqual(row['status'], 'ambiguous')
        self.assertNotIn('deadline_to_ready_ns', row)

    def test_duplicate_timer_is_ambiguous(self):
        row = self.join(timers=[self.timer, copy.deepcopy(self.timer)])
        self.assertEqual(row['timer_scan_match_count'], 2)
        self.assertNotIn('deadline_to_timer_arrival_ns', row)

    def test_wrong_deadline_and_wrong_order_cannot_join(self):
        for key, value in [('deadline_tick_ms', 140000),
                           ('has_deadline', False),
                           ('dispatch_ns', self.cycle['wake_ns'] + 1),
                           ('ready_ns', self.cycle['after_deadline_ns'] - 1)]:
            event = dict(self.event, **{key: value})
            self.assertEqual(self.join(events=[event])['status'], 'unmatched')

    def test_queue_conservation_rejects_corruption(self):
        with self.assertRaisesRegex(ValueError, 'queue mismatch'):
            self.join(events=[dict(self.event, queue_ns=1)])

    def test_before_origin_rejected(self):
        with self.assertRaisesRegex(ValueError, 'predates'):
            deadline_ns(self.clock, 753)

    def test_stale_intermediate_cannot_attach_reservation(self):
        cycle = dict(self.cycle, worked=True)
        reservation = dict(worker_records=[dict(index=0, record=copy.deepcopy(cycle))],
                           current_service_record_indices=[0],
                           current=dict(before_copy_ns=cycle['wake_ns'] + 1,
                                        after_position_ns=cycle['end_ns'] - 1))
        joined = dict(events=[reservation])
        validate_service_join([cycle], joined)
        stale = copy.deepcopy(joined)
        stale['events'][0]['worker_records'][0]['record']['wake_ns'] += 1
        with self.assertRaisesRegex(ValueError, 'stale service join worker'):
            validate_service_join([cycle], stale)
        reservation['current']['after_position_ns'] = cycle['end_ns'] + 1
        with self.assertRaisesRegex(ValueError, 'stale service join reservation'):
            validate_service_join([cycle], joined)


if __name__ == '__main__':
    unittest.main()
