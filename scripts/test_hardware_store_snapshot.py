#!/usr/bin/env python3
"""Test fixed-file, read-only store snapshot protocol on disposable files."""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from hardware_bulk_protocol import download


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--program', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    a = p.parse_args()
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    source_dir = out / 'system'; source_dir.mkdir()
    source = source_dir / 'STORE.IMG'; data = bytes(range(251)) * 5000
    source.write_bytes(data)
    key, boot, tx = uuid.uuid4().hex, uuid.uuid4().hex, uuid.uuid4().hex
    (out / 'key').write_text(key)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0)); port = listener.getsockname()[1]
    env = {**os.environ, 'BROOK_BULK_TEST_ROOT': str(source_dir),
           'BROOK_BULK_TEST_KEY': str(out / 'key'), 'BROOK_BULK_TEST_PORT': str(port)}
    with (out / 'guest.log').open('w') as log:
        process = subprocess.Popen([str(a.program.resolve()), boot, tx, 'get', uuid.uuid4().hex],
                                   env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            end = time.monotonic() + 10
            while 'HWBULK_LISTENING' not in (out / 'guest.log').read_text():
                if process.poll() is not None or time.monotonic() >= end:
                    raise RuntimeError('receiver did not listen')
                time.sleep(.02)
            result = download('127.0.0.1', port, key, boot, tx, out / 'snapshot.img',
                              max_bytes=4026531840, timeout=30)
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.terminate(); process.wait(timeout=5)
    assert (out / 'snapshot.img').read_bytes() == data == source.read_bytes()
    assert result['guest_path'] == '/media/system/STORE.IMG'
    assert result['retention'] == 'system_store_readonly'
    assert subprocess.run([str(a.program.resolve()), boot, uuid.uuid4().hex, 'put'],
                          env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 2
    print('STORE_SNAPSHOT_HOST_PASS')


if __name__ == '__main__':
    main()
