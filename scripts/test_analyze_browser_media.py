import unittest
from analyze_browser_media import analyze


def sample(source, arrival, media, frames, drops=0):
    return {'monotonic': arrival, 'state': {'phase': 'test', 'paused': False,
            'performance_ms': source * 1000, 't': media, 'duration': 30,
            'frames': frames, 'dropped': drops}}


class BrowserMediaTests(unittest.TestCase):
    def test_delayed_reordered_receipts_do_not_define_playback_rate(self):
        rows = [sample(1, 10, 1, 24), sample(3, 10.01, 3, 72), sample(2, 20, 2, 48)]
        result = analyze(rows, 'test')
        self.assertEqual(result['out_of_order_receipts'], 1)
        self.assertEqual(result['media_seconds_per_page_second'], 1)
        self.assertEqual(result['total_video_frames_per_page_second'], 24)
        self.assertEqual(result['source_span_seconds'], 2)
        self.assertEqual(result['receipt_span_seconds'], 10)

    def test_loop_is_unwrapped_but_small_backstep_remains_visible(self):
        r = analyze([sample(1, 1, 29, 24), sample(3, 3, 1, 72), sample(4, 4, .9, 96)], 'test')
        self.assertEqual(r['media_loops'], 1)
        self.assertEqual(r['media_backsteps'], 1)
        self.assertAlmostEqual(r['media_progress_seconds'], 1.9)

    def test_receipt_interval_cannot_prove_capture_alignment(self):
        rows = [sample(1, 11, 1, 24), sample(2, 12, 2, 48), sample(3, 13, 3, 72)]
        r = analyze(rows, 'test', (10, 12))
        self.assertEqual(r['samples'], 2)
        self.assertIn('does not align', r['scope'])

    def test_missing_span_and_counter_reset_are_rejected(self):
        for rows in ([sample(1, 1, 1, 24)],
                     [sample(1, 1, 1, 24), sample(1, 2, 2, 48)],
                     [sample(1, 1, 1, 24), sample(2, 2, 2, 0)]):
            with self.assertRaises(ValueError): analyze(rows, 'test')


if __name__ == '__main__':
    unittest.main()
