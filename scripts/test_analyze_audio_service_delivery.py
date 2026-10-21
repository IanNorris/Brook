#!/usr/bin/env python3
"""Check conservative diagnostic interpretation, including raw-byte rounding."""
import copy
import unittest
from workloads.analyze_audio_service_delivery import analyze


def fixture():
    previous = dict(hardware_epoch=1, before_copy_ns=1_000_000_000,
                    after_copy_ns=1_000_000_001, after_position_ns=1_000_000_002,
                    pending_before_reserve=14592, reserved_bytes=0,
                    lead_before_bytes=14592, accepted_bytes=1792, advanced_bytes=0,
                    running=True, position_valid=True, publish_safe=True)
    current = dict(hardware_epoch=1, before_copy_ns=1_025_000_000,
                   after_copy_ns=1_025_000_001, after_position_ns=1_025_000_002,
                   pending_before_reserve=11520, reserved_bytes=768,
                   lead_before_bytes=12288, accepted_bytes=7056, advanced_bytes=0,
                   running=True, position_valid=True, publish_safe=True)
    service_reset = dict(epoch=5, enabled=True, cycles=0, worker_pid=15, worker_incarnation=15)
    delivery_reset = dict(epoch=5, enabled=True, observations=0)
    service = dict(epoch=6, enabled=False, worker_pid=15, worker_incarnation=15,
                   invalid_clock=0, contended_records=0, saturated=False,
                   cycles=2, slow_cycles=0, excluded_slow=0, events=[])
    delivery = dict(epoch=6, enabled=False, invalid_clock=0, contended_records=0,
                    saturated=False, invalid_position=0, unsafe=0,
                    reservations=1, reservations_retained=1, reservations_overwritten=0,
                    reserved_bytes=768, reservation_events=[dict(control_epoch=5, observation=2,
                        has_previous=True, previous=previous, current=current)])
    return [dict(command=command, result=result) for command, result in (
        ('audio timing reset', service_reset), ('audio delivery reset', delivery_reset),
        ('audio delivery off', delivery), ('audio timing off', service))]


class DiagnosticTests(unittest.TestCase):
    def test_position_compression_is_not_worker_attribution(self):
        report = analyze(fixture(), 12288)
        row = report['reservations'][0]
        self.assertEqual(row['position_advance']['advance_bytes_bounds'], [4864, 4867])
        self.assertEqual(row['position_advance']['position_read_elapsed_upper_ns'], 24_999_999)
        self.assertTrue(row['position_advance']['exceeds_nominal_upper'])
        self.assertEqual(row['worker_overlaps'], [])
        self.assertIn('unknown', row['status'])

    def test_rounding_can_make_uncorrected_lower_bound_negative(self):
        for remainder in (1, 2, 3):
            with self.subTest(remainder=remainder):
                rows = fixture()
                rows[2]['result']['reservation_events'][0]['previous']['advanced_bytes'] = 4864 + remainder
                position = analyze(rows, 12288)['reservations'][0]['position_advance']
                self.assertEqual(position['advance_bytes_bounds'], [0, 3-remainder])
                self.assertFalse(position['exceeds_nominal_upper'])

    def test_negative_upper_bound_is_inconsistent(self):
        rows = fixture()
        rows[2]['result']['reservation_events'][0]['previous']['advanced_bytes'] = 4868
        with self.assertRaisesRegex(ValueError, 'position advance'):
            analyze(rows, 12288)

    def test_no_previous_sample_has_no_position_inference(self):
        rows = fixture()
        rows[2]['result']['reservation_events'][0].update(has_previous=False, previous=None)
        row = analyze(rows, 12288)['reservations'][0]
        self.assertNotIn('position_advance', row)

    def test_omitted_records_reject_adjacent_submission_inference(self):
        rows = fixture()
        rows[2]['result']['contended_records'] = 1
        with self.assertRaisesRegex(ValueError, 'omitted'):
            analyze(rows, 12288)

    def test_worker_generation_must_match(self):
        rows = fixture()
        rows[3]['result']['worker_incarnation'] = 16
        with self.assertRaisesRegex(ValueError, 'identity'):
            analyze(rows, 12288)

    def test_reservation_must_equal_lead_deficit(self):
        rows = fixture()
        rows[2]['result']['reservation_events'][0]['current']['pending_before_reserve'] += 4
        with self.assertRaisesRegex(ValueError, 'lead deficit'):
            analyze(rows, 12288)

    def test_incomplete_optional_mix_controls_are_rejected(self):
        rows = fixture()
        rows.append(dict(command='audio contributions off', result={}))
        with self.assertRaisesRegex(ValueError, 'mix control'):
            analyze(rows, 12288)

    def test_duplicate_control_is_rejected(self):
        rows = fixture()
        rows.append(copy.deepcopy(rows[0]))
        with self.assertRaisesRegex(ValueError, 'duplicate control'):
            analyze(rows, 12288)


if __name__ == '__main__':
    unittest.main()
