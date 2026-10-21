"""Check media transfer evidence and the real scheduler fixture's HTTP contract."""
import hashlib
import http.client
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SERVER = Path(__file__).parent / 'workloads/serve_browser_sched_mix.py'
spec = importlib.util.spec_from_file_location('scheduler_media_server', SERVER)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class TransferTests(unittest.TestCase):
    def test_timeout_retains_only_completed_writes_and_uncertain_block(self):
        payload = b'a' * 65536 + b'b' * 300

        class Interrupted:
            calls = 0

            def write(self, block):
                self.calls += 1
                if self.calls == 2:
                    raise TimeoutError('slow reader')
                return len(block)

        record = {}
        with self.assertRaises(TimeoutError):
            module.copy_media_stream(io.BytesIO(payload), Interrupted(), record)
        self.assertFalse(record['complete'])
        self.assertEqual(record['completed_write_bytes'], 65536)
        self.assertEqual(record['uncertain_write_bytes'], 300)
        self.assertEqual(record['completed_write_sha256'], hashlib.sha256(payload[:65536]).hexdigest())
        self.assertIn('slow reader', record['error'])
        self.assertLessEqual(record['start_ns'], record['end_ns'])

    def test_short_write_cannot_be_reported_as_complete(self):
        class ShortWriter:
            def write(self, block):
                return 3

        record = {}
        with self.assertRaises(OSError):
            module.copy_media_stream(io.BytesIO(b'abcdef'), ShortWriter(), record)
        self.assertFalse(record['complete'])
        self.assertEqual(record['completed_write_bytes'], 3)
        self.assertEqual(record['uncertain_write_bytes'], 3)
        self.assertEqual(record['completed_write_sha256'], hashlib.sha256(b'abc').hexdigest())

    def test_real_http_content_length_hash_and_ignored_range(self):
        with tempfile.TemporaryDirectory(prefix='brook-media-transfer-') as tmp:
            root = Path(tmp)
            clip = root / 'source.mp4'
            payload = bytes(range(256)) * 1025
            clip.write_bytes(payload)
            output = root / 'fixture'
            process = subprocess.Popen(
                [sys.executable, str(SERVER), '--output', str(output), '--clip', str(clip),
                 '--media-write-timeout', '60'],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                self.assertTrue(process.stdout.readline().startswith('BROWSER_MIX_SERVER '))
                identity = json.loads((output / 'server.json').read_text())
                self.assertEqual(identity['media_write_timeout_s'], 60)
                connection = http.client.HTTPConnection('127.0.0.1', identity['port'], timeout=5)
                connection.request('GET', '/clip.mp4', headers={'Range': 'bytes=10-20'})
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                self.assertEqual(int(response.getheader('Content-Length')), len(payload))
                self.assertEqual(response.read(), payload)
                connection.close()
            finally:
                if output.exists():
                    (output / 'stop').touch()
                try:
                    stdout, stderr = process.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.communicate(timeout=5)
                    raise
            self.assertEqual(process.returncode, 0, stderr)
            rows = [json.loads(line) for line in (output / 'media-transfers.jsonl').read_text().splitlines()]
            self.assertEqual(len(rows), 1)
            row = rows[0]
            self.assertTrue(row['complete'])
            self.assertEqual(row['range_request'], 'bytes=10-20')
            self.assertEqual(row['source_size_bytes'], len(payload))
            self.assertEqual(row['completed_write_bytes'], len(payload))
            self.assertEqual(row['uncertain_write_bytes'], 0)
            self.assertEqual(row['completed_write_sha256'], hashlib.sha256(payload).hexdigest())
            self.assertEqual(row['completed_write_sha256'], row['source_sha256'])

    def test_timeout_option_is_bounded(self):
        for value in ('0', '-1', 'nan', 'inf', '301'):
            result = subprocess.run(
                [sys.executable, str(SERVER), '--output', 'unused', '--clip', 'unused',
                 '--media-write-timeout', value], capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('--media-write-timeout must be', result.stderr)


if __name__ == '__main__':
    unittest.main()
