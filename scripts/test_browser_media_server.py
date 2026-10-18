"""Exercise report retention and same-origin fixture restart over real HTTP."""
import http.client
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MediaFixtureTests(unittest.TestCase):
    def test_long_report_and_checked_same_origin_restart(self):
        (ROOT / 'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as tmp:
            root = Path(tmp)
            clip = root / 'source.webm'
            clip.write_bytes(b'fixture-media')
            output = root / 'fixture'
            command = [sys.executable, str(ROOT / 'scripts/workloads/browser_media_server.py'),
                       '--output', str(output), '--clip', str(clip)]

            def start(resume=False):
                process = subprocess.Popen(command + (['--resume'] if resume else []),
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                self.addCleanup(self.cleanup_process, process)
                self.assertTrue(process.stdout.readline().startswith('READY '))
                return process, json.loads((output / 'servers.json').read_text())['media_port']

            def stop(process):
                (output / 'stop-server').touch()
                stdout, stderr = process.communicate(timeout=10)
                self.assertEqual(process.returncode, 0, stderr)
                self.assertIn('SERVER_EXIT_0', stdout)

            first, port = start()
            body = json.dumps({'phase': 'long-report', 'samples': 'x' * 400000})
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
            connection.request('POST', '/state', body, {'Content-Type': 'application/json'})
            response = connection.getresponse()
            self.assertEqual(response.status, 204)
            response.read()
            connection.request('POST', '/state', '{}', {'Content-Length': str(4 * 1024 * 1024 + 1)})
            self.assertEqual(connection.getresponse().status, 400)
            connection.close()
            stop(first)
            second, resumed_port = start(resume=True)
            self.assertEqual(resumed_port, port)
            self.assertFalse((output / 'stop-server').exists())
            rows = (output / 'media-state.jsonl').read_text().splitlines()
            self.assertEqual(len(rows), 1)
            self.assertEqual(json.loads(rows[0])['state'], json.loads(body))
            stop(second)
            # A different source must not silently inherit the old origin.
            clip.write_bytes(b'changed-media')
            rejected = subprocess.run(command + ['--resume'], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn('matching media/page', rejected.stderr)
            self.assertTrue((output / 'stop-server').exists())

    @staticmethod
    def cleanup_process(process):
        if process.poll() is None:
            process.terminate()
        process.communicate(timeout=10)


if __name__ == '__main__':
    unittest.main()
