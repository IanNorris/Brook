#!/usr/bin/env python3
"""Stage named, verified SSD package files to an already attached Dell USB.

This only stages files; it never installs or reboots. Each transfer is one-shot
and journalled so an ambiguous operation is inspected, never replayed blindly.
"""
import argparse
import json
import os
from pathlib import Path
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tools/vm_bridge'), str(ROOT / 'scripts')]
from hardware import Hardware
from hardware_bulk_protocol import upload
from pack_ssd_install_source import verify_package


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--session', required=True)
    p.add_argument('--key-file', required=True, type=Path)
    p.add_argument('--helper', required=True)
    p.add_argument('--package', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--file', required=True, choices=['INST0000.IMG', 'INST0001.IMG', 'INST0002.IMG', 'INST0003.IMG', 'INSTHASH.BIN'])
    a = p.parse_args()
    manifest = verify_package(a.package)
    entries = {i['name']: i for i in manifest['segments']}
    if a.file != 'INSTHASH.BIN' and a.file not in entries:
        raise ValueError('file not present in verified package')
    out = a.output.absolute(); out.mkdir(parents=True, exist_ok=False, mode=0o700)
    parent_fd = os.open(out.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(parent_fd)
    finally:
        os.close(parent_fd)
    h = Hardware(); session = h.load_session(a.session); boot = session['guest_boot_id']
    doctor = h.doctor(a.session)
    if not doctor.get('reachable') or not doctor.get('identity_matches'):
        raise RuntimeError('Dell boot is not reachable or has changed')
    key = a.key_file.read_text()[:32]
    tx = uuid.uuid4().hex
    receipt = {'state': 'prepared', 'session': a.session, 'guest_boot_id': boot,
               'transaction_id': tx, 'source': str((a.package / a.file).resolve()), 'filename': a.file}
    def save(state, **details):
        receipt.update(state=state, **details)
        tmp = out / 'receipt.tmp'
        with tmp.open('w') as stream:
            json.dump(receipt, stream, indent=2); stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
        tmp.replace(out / 'receipt.json')
        directory = os.open(out, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    try:
        save('dispatching')
        h.guest_run(a.session, tx, [a.helper, boot, tx, 'put', a.file], cwd='/tmp', uid=0, wait_s=2)
        deadline = time.monotonic() + 90
        while True:
            job = h.job(a.session, tx, wait_s=2)
            if 'HWBULK_LISTENING' in job['stdout']['text']:
                break
            if job['state'] in ('failed', 'completed', 'lost', 'rejected') or time.monotonic() >= deadline:
                raise RuntimeError('receiver did not listen; inspect the existing job')
        save('receiving')
        result = upload(session['host'], 1236, key, boot, tx, a.package / a.file,
                        max_bytes=2 * 1024**3, timeout=3600)
        save('published', transfer=result)
        job = h.job(a.session, tx, wait_s=20)
        if job.get('exit', {}).get('code') != 0:
            raise RuntimeError('transfer reply received but guest job did not complete cleanly')
        if result['guest_path'] != '/media/usb/' + a.file:
            raise RuntimeError('unexpected target path')
        expected = entries[a.file]['sha256'] if a.file in entries else manifest['install_record_sha256']
        if result['sha256'] != expected:
            raise RuntimeError('completed transfer does not match verified package')
        save('completed', job=job)
        print(json.dumps({'state': 'completed', 'filename': a.file, 'transaction_id': tx,
                          'bytes': result['bytes'], 'sha256': result['sha256']}), flush=True)
    except Exception as error:
        save('needs_inspection', error=str(error), target_file_may_exist=True,
             recovery='Inspect this transaction and target file; do not replay automatically.')
        raise


if __name__ == '__main__':
    main()
