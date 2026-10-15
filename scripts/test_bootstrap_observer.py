"""Acceptance monitoring must survive ancillary profiling transport failures."""
import contextlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import bootstrap_acceptance as acceptance


class ObserverTests(unittest.TestCase):
    def run_observer(self, failed_action, complete=True):
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
                        log.write('100% tests passed, 0 tests failed out of 13\n' * (2 if complete else 1))
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


if __name__ == '__main__':
    unittest.main()
