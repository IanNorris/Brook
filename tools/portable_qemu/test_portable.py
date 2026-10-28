#!/usr/bin/env python3
"""Contract tests for host portability and authenticated VM control."""
import http.client
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest

spec = importlib.util.spec_from_file_location('launch', Path(__file__).with_name('launch.py'))
launch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launch)


class Portable(unittest.TestCase):
    def test_startup_failure_includes_qemu_stderr(self):
        with tempfile.TemporaryDirectory() as directory:
            log_path = Path(directory) / 'qemu.log'
            with log_path.open('w') as log:
                with subprocess.Popen(
                    [sys.executable, '-c',
                     'import sys; print("QEMU: display backend unavailable", file=sys.stderr); sys.exit(1)'],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True,
                ) as proc:
                    with self.assertRaisesRegex(RuntimeError, 'QEMU: display backend unavailable') as caught:
                        launch.startup_qmp(proc, log_path)
                    self.assertIn(str(log_path), str(caught.exception))

    def test_cross_isa_and_cpu_fallback(self):
        self.assertEqual(launch.accelerators('Darwin', 'arm64'), [('tcg', 'max')])
        self.assertEqual(launch.accelerators('Linux', 'x86_64'), [('kvm', 'host'), ('tcg', 'max')])
        self.assertEqual(launch.accelerators('Darwin', 'x86_64')[0], ('hvf', 'host'))
        self.assertEqual(launch.accelerators('Windows', 'AMD64')[0], ('whpx', 'max,vmx=off'))
        cmd = launch.command('C:/QEMU λ, tools/qemu.exe', 'tcg', 'max', 2, 8192, 'none', 'none', 'a' * 32)
        self.assertEqual(cmd[0], 'C:/QEMU λ, tools/qemu.exe')
        self.assertNotIn('host', cmd)
        self.assertTrue(all('C:/' not in item for item in cmd[1:]))
        self.assertNotIn('hostfwd', ' '.join(cmd))
        self.assertNotIn('gl=on', ' '.join(cmd))

    def test_control_auth_and_generation(self):
        class Process:
            def poll(self):
                return None
        class QMP:
            def __init__(self):
                self.calls = []
            def call(self, command, arguments=None):
                self.calls.append(command)
                return {'status': 'running'}
        with tempfile.TemporaryDirectory(prefix='Brook spaces λ, ') as directory:
            qmp = QMP()
            control = launch.Control(Process(), qmp, Path(directory), 'a' * 32, None)
            server = launch.serve(control, 'private-token')
            try:
                connection = http.client.HTTPConnection('127.0.0.1', server.server_port)
                connection.request('POST', '/control', json.dumps({'action': 'force_stop', 'generation': 1, 'acknowledge_data_loss': True}))
                reply = connection.getresponse()
                self.assertEqual(reply.status, 403)
                reply.read()
                self.assertEqual(qmp.calls, [])
                with self.assertRaisesRegex(ValueError, 'Stale'):
                    control.dispatch(dict(action='input', generation=0, events=[{}]))
                with self.assertRaisesRegex(ValueError, 'acknowledge'):
                    control.dispatch(dict(action='force_stop', generation=1))
                self.assertEqual(qmp.calls, [])
                connection = http.client.HTTPConnection('127.0.0.1', server.server_port)
                connection.request('POST', '/control', json.dumps({'action': 'status'}), {'Authorization': 'Bearer private-token'})
                reply = connection.getresponse()
                self.assertEqual(reply.status, 200)
                self.assertEqual(json.loads(reply.read())['result']['generation'], 1)
                with control.lock:
                    done = threading.Event()
                    observer = threading.Thread(target=lambda: (control.dispatch({'action': 'status'}), done.set()), daemon=True)
                    observer.start()
                    self.assertTrue(done.wait(1), 'Status must remain available during a pending shutdown')
            finally:
                server.shutdown()
                server.server_close()

    def test_failed_sync_never_commits_power(self):
        class Control(launch.Control):
            def lifecycle(self, verb, request, boot):
                self.verbs.append(verb)
                return dict(state='ok' if verb == 'hello' else 'failed', result=0 if verb == 'hello' else -5, boot_id='b' * 32)
        with tempfile.TemporaryDirectory() as directory:
            control = Control(None, None, Path(directory), 'a' * 32, None)
            control.verbs = []
            with self.assertRaises(RuntimeError):
                control.dispatch(dict(action='shutdown', generation=1))
            self.assertNotIn('commit', control.verbs)
            receipt = json.loads(next(Path(directory).glob('power-*.json')).read_text())
            self.assertEqual(receipt['completion'], 'unknown')


if __name__ == '__main__':
    unittest.main()
