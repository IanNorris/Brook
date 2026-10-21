#!/usr/bin/env python3
import unittest
from workloads.analyze_qemu_audio_cadence import analyze


def write(ms, filled, avail, index, size):
    return f'1970-01-01T00:00:00.{ms*1000:06d}Z pw_write filled={filled} avail={avail} index={index} len={size}'


def fixture():
    return '\n'.join([write(0, 0, 100, 0, 60), write(10, 20, 80, 60, 120),
                      '1970-01-01T00:00:00.015000Z audio_timer_delayed interval 12 ms',
                      write(20, 30, 70, 140, 0)])


class CadenceTests(unittest.TestCase):
    def test_clamped_write_and_queue_conservation(self):
        report = analyze(fixture(), 0, 30_000_000)
        self.assertEqual(report['total_accepted_bytes'], 140)
        self.assertEqual(report['removed_between_observed_writes_bytes'], 110)
        self.assertEqual(report['positive_write_gap']['max_ns'], 10_000_000)
        self.assertEqual(report['delayed_timers'][0]['reported_virtual_interval_ms'], 12)

    def test_index_wrap_is_valid(self):
        text = '\n'.join([write(0, 0, 100, (1 << 32)-6, 10), write(10, 5, 95, 4, 0)])
        self.assertEqual(analyze(text, 0, 30_000_000)['removed_between_observed_writes_bytes'], 5)

    def test_missing_write_or_voice_restart_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'index discontinuity'):
            analyze(fixture().replace('index=140', 'index=0'), 0, 30_000_000)

    def test_fill_cannot_increase_without_a_write(self):
        text = '\n'.join([write(0, 0, 100, 0, 10), write(10, 20, 80, 10, 0)])
        with self.assertRaisesRegex(ValueError, 'fill grew'):
            analyze(text, 0, 30_000_000)

    def test_negative_ring_state_is_not_silently_clamped(self):
        with self.assertRaisesRegex(ValueError, 'invalid PipeWire ring'):
            analyze(fixture().replace('filled=20 avail=80', 'filled=-1 avail=101'), 0, 30_000_000)

    def test_bad_event_format_is_not_ignored(self):
        with self.assertRaisesRegex(ValueError, 'unparsed audio'):
            analyze(fixture()+'\npw_write missing timestamp', 0, 30_000_000)

    def test_common_window_excludes_transition_events(self):
        report = analyze(fixture(), 1_000_000, 30_000_000)
        self.assertEqual(len(report['writes']), 2)
        self.assertEqual(report['excluded_transition_events']['prefix'], 1)


if __name__ == '__main__':
    unittest.main()
