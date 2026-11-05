#!/usr/bin/env python3
"""Verify panic messages survive structured decoding and terminal escaping."""
from contextlib import redirect_stdout
from io import StringIO
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import crash_decoder as decoder

reason = 'storage assertion\n\x1b[31merror'
raw = reason.encode('utf-8')
packet = struct.pack('<BBBBI', decoder.PANIC_MAGIC, decoder.PANIC_VERSION_RAW, 0, 1, decoder.PANIC_PAD)
packet += struct.pack('<II', decoder.PKT_MESSAGE, len(raw)) + raw
log = b'\x00disk ready\n'
packet += struct.pack('<II', decoder.PKT_LOG_TAIL, len(log)) + log
out = StringIO()
with redirect_stdout(out):
    decoder.decode_crash(packet, None, False, True)
decoded=json.loads(out.getvalue())
assert decoded['message'] == reason
assert decoded['log_tail'] == {'available': True, 'text': 'disk ready\n'}
out = StringIO()
with redirect_stdout(out):
    decoder.decode_crash(packet, None, False, False)
assert 'storage assertion\\n\\u001b[31merror' in out.getvalue()
assert 'disk ready' in out.getvalue()
print('panic message decoder: PASS')
