#!/usr/bin/env python3
"""Host-only tests for the captured-volume phase checker."""
import unittest

from audio_control_wave import has_ordered_phases, steady_tone_frames, volume_phase_runs


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


class SteadyToneTest(unittest.TestCase):
    def test_exact_stereo_tone_measures_longest_run(self):
        period = [-8000, -8000] * 50 + [8000, 8000] * 50
        capture = [0, 0] * 10 + period * 10 + [8000, -8000] * 20 + period * 2
        self.assertEqual(steady_tone_frames(capture), 1000)

    def test_wrong_frequency_or_unmatched_stereo_is_rejected(self):
        for capture in (([-8000, -8000] * 49 + [8000, 8000] * 49) * 10,
                        ([-8000, 8000] * 50 + [8000, -8000] * 50) * 10):
            self.assertEqual(steady_tone_frames(capture), 0)


if __name__ == '__main__':
    unittest.main()
