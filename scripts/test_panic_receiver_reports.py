#!/usr/bin/env python3
"""Keep separate optical reports even when timestamps and names collide."""
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import panic_receiver

class FixedTime:
    @classmethod
    def now(cls):
        return cls()
    def strftime(self, unused):
        return 'fixed'

with tempfile.TemporaryDirectory(prefix='brook-panic-reports-') as tmp:
    names = iter(['a'*32, 'a'*32, 'b'*32])
    with patch.object(panic_receiver, 'datetime', FixedTime), \
         patch.object(panic_receiver.uuid, 'uuid4', side_effect=lambda: SimpleNamespace(hex=next(names))):
        first = panic_receiver.save_raw_crash(Path(tmp), 'FIRST')
        second = panic_receiver.save_raw_crash(Path(tmp), 'SECOND')
    assert first != second and first.read_text() == 'FIRST' and second.read_text() == 'SECOND'
    assert first.with_suffix('.json') != second.with_suffix('.json')
print('panic receiver unique reports: PASS')
