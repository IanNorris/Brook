import unittest

from analyze_client_scanout_timeline import FIELDS, load_rows, summarize


def row(sequence, latch=0, retire=0, successor=0):
    return dict(
        sequence=sequence, owner=1, offer=sequence, buffer=sequence, worker="bsp",
        admitted_us=10 * sequence, flush_start_us=0, flush_end_us=0,
        arm_us=10 * sequence + 1, latch_us=latch, retired_us=retire,
        retired_by=successor, retirement="replacement" if retire else "held",
        latch_observer="offworker" if latch else "none",
    )


def snapshot(*rows):
    columns = sorted(FIELDS)
    return {
        "reply": {
            "columns": columns, "count": len(rows), "clock": "monotonic",
            "rows": [[entry[field] for field in columns] for entry in rows],
        }
    }


class TestTimeline(unittest.TestCase):
    def test_overlap_joins_named_successor(self):
        capture = [
            snapshot(row(1)),
            snapshot(row(1, 15, 38, 3), row(2, 25), row(3)),
            snapshot(row(3, 37)),
        ]
        report = summarize(load_rows(capture))
        self.assertEqual(report["unique_rows"], 3)
        self.assertEqual(report["replacement_retire_after_latch"]["p50_us"], 1)
        self.assertFalse(report["unresolved_replacements"])
        self.assertFalse(report["inconsistencies"])

    def test_adjacent_latch_cannot_mask_early_release(self):
        report = summarize(load_rows(snapshot(row(1, 15, 33, 3), row(2, 25), row(3, 37))))
        self.assertEqual(report["inconsistencies"], [
            {"sequence": 1, "reason": "retired_before_latch", "successor": 3}
        ])

    def test_uncaptured_successor_is_unresolved(self):
        report = summarize(load_rows(snapshot(row(1, 15, 35, 3), row(2, 25))))
        self.assertEqual(report["unresolved_replacements"], [{"sequence": 1, "successor": 3}])
        self.assertIsNone(report["replacement_retire_after_latch"])

    def test_reject_regression_and_mixed_identity(self):
        with self.assertRaisesRegex(ValueError, "Sequence reset"):
            load_rows([snapshot(row(2)), snapshot(row(1))])
        with self.assertRaisesRegex(ValueError, "Conflicting identity"):
            load_rows([snapshot(row(1)), snapshot(dict(row(1), buffer=9))])
        with self.assertRaisesRegex(ValueError, "Conflicting recorded event"):
            load_rows([snapshot(row(1, 15)), snapshot(row(1, 16))])


if __name__ == "__main__":
    unittest.main()
