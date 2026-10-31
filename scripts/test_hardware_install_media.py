#!/usr/bin/env python3
"""Exercise the direct-to-media receiver on a disposable host directory."""
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
from hardware_bulk_protocol import upload


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--program', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    store = out / 'media'; store.mkdir()
    key, boot = uuid.uuid4().hex, uuid.uuid4().hex
    keyfile = out / 'key'; keyfile.write_text(key)
    source = out / 'input'; source.write_bytes(bytes(range(256)) * 512)

    def run(name, finalize=True, expected=0):
        tx = uuid.uuid4().hex
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1', 0)); port = reserve.getsockname()[1]
        env = {**os.environ, 'BROOK_BULK_TEST_ROOT': str(store),
               'BROOK_BULK_TEST_KEY': str(keyfile), 'BROOK_BULK_TEST_PORT': str(port)}
        with (out / (tx + '.log')).open('w') as log:
            child = subprocess.Popen([str(a.program.resolve()), boot, tx, 'put', name],
                                     env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 10
                while 'HWBULK_LISTENING' not in (out / (tx + '.log')).read_text():
                    if child.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError('receiver did not listen')
                    time.sleep(.01)
                try:
                    result = upload('127.0.0.1', port, key, boot, tx, source, finalize)
                    if expected:
                        raise AssertionError('expected refusal')
                    if finalize:
                        assert result['guest_path'] == '/media/usb/' + name
                except RuntimeError as error:
                    if not expected or error.args[0]['code'] != expected:
                        raise
                except ConnectionResetError:
                    # The receiver may refuse an existing target before the
                    # sender has finished writing the body to the socket.
                    if not expected:
                        raise
                assert child.wait(timeout=10) == (expected if expected else (0 if finalize else 19))
            finally:
                if child.poll() is None:
                    child.terminate(); child.wait(timeout=5)

    run('INST0000.IMG')
    assert (store / 'INST0000.IMG').read_bytes() == source.read_bytes()
    run('INST0000.IMG', expected=14)
    assert (store / 'INST0000.IMG').read_bytes() == source.read_bytes()
    run('INST0001.IMG', finalize=False)
    assert not (store / 'INST0001.IMG').exists()
    print('INSTALL_MEDIA_TRANSFER_HOST_PASS', flush=True)


if __name__ == '__main__':
    main()
