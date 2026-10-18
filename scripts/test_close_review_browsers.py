import importlib.util
from pathlib import Path
import tempfile
import unittest

path = Path(__file__).parent / "workloads/close_review_browsers.py"
spec = importlib.util.spec_from_file_location("close_review_browsers", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def snapshot(windows, tasks, truncated=False):
    return {"components": {
        "windows": {"state": "ok", "result": {"windows": windows}},
        "processes": {"state": "ok", "result": {"tasks": tasks, "truncated": truncated}}}}


class BrowserExitTests(unittest.TestCase):
    def test_chromium_browser_and_background_children_must_both_drain(self):
        reference = {"window_id": 3, "incarnation": "2", "guest_boot_id": "epoch"}
        window = {"app_id": "chromium-browser", "launch_id": 2, "reference": reference}
        child = {"pid": 37, "launch_id": "2", "name": "utility", "kernel": False, "state": "terminated"}
        sequence = [snapshot([window], [child]), snapshot([], [child]), snapshot([], [])]
        class Bridge:
            observations = 0
            closes = []
            def observe(self, *args, **kwargs):
                result = sequence[self.observations]; self.observations += 1; return result
            def window_action(self, *args, **kwargs):
                self.closes.append(args); return {"state": "applied"}
        bridge = Bridge()
        with tempfile.TemporaryDirectory() as temporary:
            result = module.close_browsers(bridge, "session", temporary, sleep=lambda _: None)
        self.assertTrue(result["ready_for_checked_stop"])
        self.assertEqual(bridge.observations, 3)
        self.assertEqual(len(bridge.closes), 1)
        self.assertEqual(bridge.closes[0][1], reference)

    def test_closed_window_with_hidden_browser_is_not_completion(self):
        class Bridge:
            def observe(self, *args, **kwargs):
                return snapshot([], [{"pid": 43, "name": ".firefox-wrapped", "kernel": False}])
        times = iter([0, 2])
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(RuntimeError, "VM left running"):
                module.close_browsers(Bridge(), "session", temporary, 1, clock=lambda: next(times))

    def test_truncated_census_cannot_pass(self):
        class Bridge:
            def observe(self, *args, **kwargs): return snapshot([], [], truncated=True)
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(RuntimeError, "truncated"):
                module.close_browsers(Bridge(), "session", temporary)


if __name__ == "__main__":
    unittest.main()
