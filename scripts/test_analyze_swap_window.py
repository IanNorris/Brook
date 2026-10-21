#!/usr/bin/env python3
"""Boundary cases where naive cropping would hide an entire renderer stall."""
import unittest
from workloads.analyze_swap_trace import analyze_window


def trace(ends):
    return ('# brook_swap_trace_v1 count=%d dropped=0 incomplete=0 invalid_clock=0\n'
            'index,tid,begin_ns,end_ns\n' % len(ends) +
            ''.join('%d,7,%d,%d\n' % (i, end-1, end) for i, end in enumerate(ends)))


class WindowTests(unittest.TestCase):
    def test_stall_spans_entire_window(self):
        result = analyze_window(trace([10, 1000, 1010]), 100, 900)
        self.assertEqual(result['completed_swaps'], 0)
        self.assertIsNone(result['frame_time'])
        self.assertEqual(result['boundary_crossing_intervals'], [
            dict(begin_ns=10, end_ns=1000, gap_ns=990, overlap_ns=800)])

    def test_boundaries_and_complete_interval(self):
        result = analyze_window(trace([10, 20, 40, 100]), 15, 50)
        self.assertEqual(result['completed_swaps'], 2)
        self.assertEqual(result['complete_intervals'], 1)
        self.assertEqual(result['frame_time']['p50_ms'], 20/1e6)
        self.assertEqual([x['overlap_ns'] for x in result['boundary_crossing_intervals']], [5, 10])

    def test_exact_boundary_completion(self):
        result = analyze_window(trace([10, 20, 40, 100]), 20, 100)
        self.assertEqual(result['completed_swaps'], 2)
        self.assertEqual(result['complete_intervals'], 2)
        self.assertEqual(result['boundary_crossing_intervals'], [])

    def test_missing_coverage_and_dropped_rows_rejected(self):
        for text, lo, hi in [(trace([10, 20]), 5, 15),
                             (trace([10, 20]), 15, 25),
                             (trace([10, 20]), 20, 20),
                             (trace([10, 20]).replace('dropped=0', 'dropped=1'), 10, 20)]:
            with self.assertRaises(ValueError):
                analyze_window(text, lo, hi)


if __name__ == '__main__':
    unittest.main()
