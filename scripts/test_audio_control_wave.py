#!/usr/bin/env python3
"""Host-only tests for the captured-volume phase checker."""
import unittest

from audio_control_wave import has_ordered_phases, volume_phase_runs


class VolumePhasesTest(unittest.TestCase):
    def capture(self, levels):
        # Small blocks preserve the same 80%-dominance and duration rules.
        return [sample for level in levels for sample in [level] * 40]

    def test_expected_order_and_short_transition(self):
        samples = self.capture([0, 2000, 4000, 8000, 0, 2000])
        samples[40:40] = [1234] * 10
        runs = volume_phase_runs(samples, block_samples=10)
        self.assertTrue(has_ordered_phases(runs), runs)

    def test_wrong_order_is_rejected(self):
        runs = volume_phase_runs(self.capture([0, 4000, 2000, 8000, 0, 2000]), block_samples=10)
        self.assertFalse(has_ordered_phases(runs), runs)

    def test_short_levels_are_not_phases(self):
        samples = self.capture([0, 2000, 4000, 8000, 0]) + [2000] * 20
        self.assertFalse(has_ordered_phases(volume_phase_runs(samples, block_samples=10)))


if __name__ == '__main__':
    unittest.main()
