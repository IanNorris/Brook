"""Acceptance monitoring must survive ancillary profiling transport failures."""
import contextlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import bootstrap_acceptance as acceptance


class ObserverTests(unittest.TestCase):
    def run_observer(self, failed_action, complete=True, live_error=None):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            serial = directory/'serial.log'
            serial.write_text('BOOTSTRAP_BEGIN clone\n')
            clock = [0]
            actions = []

            def sleep(seconds):
                clock[0] += seconds
                if clock[0] == 6:
                    with serial.open('a') as log:
                        log.write('BOOTSTRAP_END clone 6\nBOOTSTRAP_BEGIN compile\n')
                if clock[0] == 8:
                    with serial.open('a') as log:
                        log.write('BOOTSTRAP_END compile 2\n')
                        log.write((acceptance.NATIVE_PASS_MARKER + '\n') * (2 if complete else 1))
                        log.write('BOOTSTRAP_ARTIFACTS_OK\nBOOTSTRAP_EXIT 0\n')

            def profile(_session, action, **_):
                actions.append(action)
                if action == failed_action:
                    raise TimeoutError('unknown profiling completion')
                return {}

            with patch.object(acceptance.time, 'monotonic', lambda: clock[0]), \
                 patch.object(acceptance.time, 'sleep', sleep), \
                 patch.object(acceptance.server, 'profile', profile), \
                 patch.object(acceptance.server, 'guest', return_value={}), \
                 patch.object(acceptance.server, 'load_session', return_value={}, side_effect=live_error), \
                 patch.object(acceptance.server, 'panic_state', return_value=False), \
                 contextlib.redirect_stdout(io.StringIO()):
                result = acceptance.monitor(directory, 'fixture', {'serial_log': str(serial)}, 20)
            return result, actions

    def test_export_timeout_keeps_monitoring_without_restarting_profile(self):
        (stages, snapshots, profiles), actions = self.run_observer('export')
        self.assertEqual(actions, ['start', 'export'])
        self.assertEqual([stage['name'] for stage in stages], ['clone', 'compile'])
        self.assertTrue(any(item.get('profiling_disabled') for item in snapshots))
        self.assertEqual(profiles, [])

    def test_start_timeout_is_not_retried_in_a_later_stage(self):
        (_, snapshots, profiles), actions = self.run_observer('start')
        self.assertEqual(actions, ['start'])
        self.assertTrue(any(item.get('profiling_disabled') for item in snapshots))
        self.assertEqual(profiles, [])

    def test_profiling_failure_does_not_relax_acceptance_evidence(self):
        with self.assertRaisesRegex(RuntimeError, 'missing required test/artifact evidence'):
            self.run_observer('export', complete=False)

    def test_old_suite_count_is_not_current_acceptance(self):
        old = f"100% tests passed, 0 tests failed out of {acceptance.NATIVE_TEST_COUNT - 1}\n"
        with self.assertRaisesRegex(RuntimeError, 'missing required test/artifact evidence'):
            acceptance.validate_finished_build(old * 2 + 'BOOTSTRAP_ARTIFACTS_OK\n')

    def test_stopped_vm_without_completion_fails_instead_of_waiting(self):
        with self.assertRaisesRegex(RuntimeError, 'VM process is no longer running'):
            self.run_observer('export', live_error=RuntimeError('VM process is no longer running'))


class TrackedCommandTests(unittest.TestCase):
    @staticmethod
    def status(text="", offset=0, *, state="running", live=1, eof=False, code=None):
        return {"stage": "committed", "state": state,
            "exit": {"code": code, "signal": None}, "guest": {"live_tasks": live},
            "stdout": {"text": text, "next_offset": offset, "eof": eof},
            "stderr": {"text": "", "next_offset": 0, "eof": eof}}

    def test_resume_uses_same_identity_and_byte_cursor(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            job = acceptance.TrackedCommand(directory, "fixture", ["/bin/test"])
            with patch.object(acceptance.server, "guest_run") as submit, \
                 patch.object(acceptance.server, "job_status", side_effect=[
                     self.status("one", 3),
                     self.status("two", 6, state="completed", live=0, eof=True, code=0)]) as status:
                self.assertEqual(job.poll(), "one")
                self.assertFalse(job.complete())
                resumed = acceptance.TrackedCommand(directory, "fixture", ["/bin/test"])
                self.assertEqual(resumed.poll(), "onetwo")
                self.assertTrue(resumed.complete())
                self.assertEqual(submit.call_count, 1)
                self.assertEqual(status.call_args.kwargs["stdout_offset"], 3)
            with self.assertRaisesRegex(RuntimeError, "another session or argv"):
                acceptance.TrackedCommand(directory, "different", ["/bin/test"])

    def test_exit_marker_cannot_replace_drained_lineage_and_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            job = acceptance.TrackedCommand(Path(tmp), "fixture", ["/bin/test"])
            for live, eof in [(1, True), (0, False)]:
                job.evidence["status"] = self.status("BOOTSTRAP_EXIT 0", 16,
                    state="completed", live=live, eof=eof, code=0)
                self.assertFalse(job.complete())

    def test_failure_and_missing_output_prevent_acceptance(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name in ("failure", "gap"):
                directory = Path(tmp) / name
                directory.mkdir()
                job = acceptance.TrackedCommand(directory, "fixture", ["/bin/test"])
                r = self.status(state="completed", live=0, eof=True, code=1 if name == "failure" else 0)
                if name == "gap":
                    r["stdout"]["dropped_bytes"] = 4
                with patch.object(acceptance.server, "guest_run"), \
                     patch.object(acceptance.server, "job_status", return_value=r), \
                     self.assertRaises(RuntimeError):
                    job.poll()

    def test_unknown_upload_observation_reuses_persisted_request(self):
        with tempfile.TemporaryDirectory() as tmp:
            job = acceptance.TrackedCommand(Path(tmp), "fixture", ["/bin/test"])
            with patch.object(acceptance.server, "guest_run") as submit, \
                 patch.object(acceptance.server, "job_status", side_effect=[
                     {"stage": "commit_dispatched", "state": "unknown"}, self.status()]):
                job.poll()
                self.assertFalse(job.complete())
                job.poll()
                self.assertEqual(submit.call_args_list[0], submit.call_args_list[1])


if __name__ == '__main__':
    unittest.main()
