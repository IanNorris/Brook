#!/usr/bin/env python3
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from hardware_panic_collect import PanicCollector


class Bridge:
    def __init__(self):
        self.records = [b'new panic', b'older panic']
        self.reads = 0
        self.changed = False

    def load_session(self, session):
        return {'guest_boot_id': 'a' * 32, 'host': '192.0.2.1', 'port': 1234}

    def command(self, session, command):
        data = self.records['previous' in command]
        if data is None:
            return {'recovery_panic_error': 'No validated panic record'}
        return {'recovery_panic_api': 1, 'sequence': 10 if 'previous' not in command else 9,
                'bytes': len(data), 'crc32': zlib.crc32(data), 'offset': 0, 'hex': ''}

    def recovery_panic(self, session, output, previous=False):
        self.reads += 1
        data = self.records[previous]
        Path(output).write_bytes(data)
        return {'guest_boot_id': 'a' * 32, 'sequence': (9 if previous else 10) + self.changed, 'bytes': len(data),
                'crc32': f'{zlib.crc32(data):08x}', 'sha256': hashlib.sha256(data).hexdigest()}


class TestCollector(unittest.TestCase):
    def test_both_records_are_verified_and_deduplicated(self):
        with tempfile.TemporaryDirectory() as td:
            bridge = Bridge()
            collector = PanicCollector(bridge, td)
            self.assertEqual([r['sequence'] for r in collector.collect('s')], [10, 9])
            self.assertEqual(bridge.reads, 2)
            self.assertEqual(len(list(Path(td).glob('*.json'))), 2)
            self.assertTrue(all(r['state'] == 'existing' for r in collector.collect('s')))
            self.assertEqual(bridge.reads, 2)
            file = next(Path(td).glob('panic-00000000000000000010-*.bin'))
            file.write_bytes(b'x' * len(bridge.records[0]))
            with self.assertRaisesRegex(RuntimeError, 'checksum'):
                collector.collect('s')

    def test_interrupted_receipt_and_wrong_boot(self):
        with tempfile.TemporaryDirectory() as td:
            bridge = Bridge()
            bridge.records[1] = None
            collector = PanicCollector(bridge, td)
            first = collector.collect('s')[0]
            receipt = next(Path(td).glob('*.json'))
            text = receipt.read_text()
            self.assertIn('observation only; crash boot not established', text)
            receipt.unlink()
            self.assertEqual(collector.collect('s')[0]['state'], 'existing')
            self.assertEqual(len(list(Path(td).glob('*.json'))), 1)
            self.assertEqual(bridge.reads, 1)
            Path(first['path']).unlink()
            original = bridge.recovery_panic
            def wrong_boot(*args, **kwargs):
                return original(*args, **kwargs) | {'guest_boot_id': 'b' * 32}
            bridge.recovery_panic = wrong_boot
            with self.assertRaisesRegex(RuntimeError, 'changed'):
                collector.collect('s')
            self.assertFalse(Path(first['path']).exists())

    def test_changed_snapshot_is_not_published(self):
        with tempfile.TemporaryDirectory() as td:
            bridge = Bridge()
            bridge.changed = True
            with self.assertRaisesRegex(RuntimeError, 'changed'):
                PanicCollector(bridge, td).collect('s')
            self.assertEqual(list(Path(td).iterdir()), [])

    def test_empty_or_bad_metadata(self):
        with tempfile.TemporaryDirectory() as td:
            bridge = Bridge()
            bridge.records = [None, None]
            self.assertEqual(PanicCollector(bridge, td).collect('s'), [])
            bridge.command = lambda *args: {'recovery_panic_api': 1, 'sequence': True}
            with self.assertRaisesRegex(RuntimeError, 'metadata'):
                PanicCollector(bridge, td).collect('s')


if __name__ == '__main__':
    unittest.main()
