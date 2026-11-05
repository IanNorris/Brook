#!/usr/bin/env python3
"""Verify panic messages survive structured decoding and terminal escaping."""
from contextlib import redirect_stdout, redirect_stderr
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

# A dirty ID contains a diff digest after '+'. It must not select a current
# or clean archived ELF, and its warning must not corrupt machine JSON.
system = struct.pack('<BBHQQ', 0, 1, 0, 0, 0) + b'01234567+12345678\0'.ljust(20, b'\0') + b'branch\0'.ljust(24, b'\0')
dirty_packet = packet + struct.pack('<II', decoder.PKT_SYSTEM_INFO, len(system)) + system
out, err = StringIO(), StringIO()
with redirect_stdout(out), redirect_stderr(err):
    decoder.decode_crash(dirty_packet, None, False, True)
assert json.loads(out.getvalue())['system_info']['git_hash'] == '01234567+12345678'
assert 'Dirty build' in err.getvalue()
with redirect_stderr(StringIO()):
    assert decoder.find_brook_elf('01234567+12345678') is None
    assert decoder.find_brook_elf('zzzzzzz') is None
print('panic message decoder: PASS')
