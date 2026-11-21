import unittest

from analyze_gpu_wait_profile import analyze, parse


def line(tick, kind, tid, peer, *data):
    words = " ".join(f"{value:x}" for value in data)
    return f"T {tick:x} 0 {kind} {tid:x} {peer:x} 0 {words}"


class GpuWaitAnalysis(unittest.TestCase):
    def test_trace_preserves_wait_fence_and_wake_intervals(self):
        rows = [
            line(1100, 5, 23, 23, 0x1C3, 0),
            line(1200, 10, 29, 0, 44, 1),
            line(1220, 9, 23, 29, 44, 1),
            line(1230, 1, 23, 29, 99, 0),
            line(1300, 0, 29, 23, 0x1000),
            line(1310, 7, 23, 23, 44, 110000),
            line(1320, 7, 23, 23, 44, 120000),
            line(1330, 6, 23, 23, 0x1C3, 0),
            line(1340, 5, 23, 23, 3, 9),
            line(1350, 8, 23, 29, 99, 0),
            line(1360, 1, 23, 29, 99, 0),
            line(1380, 0, 27, 23),
            line(1390, 6, 23, 23, 3, 9),
        ]
        # File export is grouped by CPU rather than sorted by timestamp.
        profile = ["PROF_BEGIN 2 0", "FRAME_CLOCK 3e8 1000 23", *reversed(rows),
                   f"PROF_END {len(rows)} 0"]
        report, timeline = analyze(parse(profile), True)
        self.assertEqual(report["waits"]["SYNCOBJ_WAIT"]["p50_us"], 230)
        self.assertEqual(report["waits"]["futex"]["p50_us"], 50)
        self.assertEqual(report["fence_resolution_to_first_observation"]["p50_us"], 110)
        self.assertEqual(report["fence_resolution_to_first_observation"]["count"], 1)
        self.assertEqual(report["fence_publication_to_gpu_wake_request"]["p50_us"], 20)
        self.assertEqual(report["wake_request_to_ready"]["gpu"]["p50_us"], 10)
        self.assertEqual(report["ready_to_run"]["gpu"]["p50_us"], 70)
        self.assertEqual(report["ready_to_run"]["futex"]["p50_us"], 20)
        self.assertEqual(sum(item["ph"] == "X" for item in timeline["traceEvents"]), 2)

    def test_partial_trace_marks_missing_pairs_without_guessing_cause(self):
        profile = [
            "FRAME_CLOCK 100 1000 23",
            line(200, 16, 23, 29, 1, 0),
            line(300, 8, 23, 30, 99, 0),
            line(310, 1, 23, 29, 99, 0),
            line(320, 0, 29, 23),
            line(330, 6, 23, 23, 0x1CA, 0),
            line(340, 5, 23, 23, 3, 0),
            "PROF_END 6 17",
        ]
        report, _ = analyze(parse(profile))
        self.assertEqual(report["dropped"], 17)
        self.assertEqual(report["pre_window_events"], 1)
        self.assertEqual(report["unpaired_wait_boundaries"], 2)
        self.assertEqual(report["wake_request_to_ready"]["futex"]["count"], 0)

    def test_malformed_count_rejected(self):
        with self.assertRaisesRegex(ValueError, "count mismatch"):
            parse(["FRAME_CLOCK 100 1000 23", "PROF_END 2 0"])


if __name__ == "__main__":
    unittest.main()
