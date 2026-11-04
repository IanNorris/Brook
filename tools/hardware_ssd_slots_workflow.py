"""Boot-pinned, one-shot SSD slot initialization; never automatically retry."""
import json
import os
from pathlib import Path
import re
import time
import uuid


class SlotRun:
    def __init__(self, bridge, output):
        self.bridge = bridge
        self.output = Path(output)
        self.receipt = {}

    def save(self, **values):
        self.receipt.update(values)
        path = self.output / 'receipt.json'; temporary = self.output / '.receipt.tmp'
        with temporary.open('w') as f:
            os.chmod(temporary, 0o600); json.dump(self.receipt, f, indent=2); f.write('\n'); f.flush(); os.fsync(f.fileno())
        temporary.replace(path)
        fd = os.open(self.output, os.O_RDONLY | os.O_DIRECTORY)
        try: os.fsync(fd)
        finally: os.close(fd)

    def job(self, session, ident, timeout=600):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            result = self.bridge.job(session, ident, wait_s=2)
            if result.get('state') in ('completed', 'failed', 'lost', 'rejected'):
                return result
            time.sleep(.5)
        raise TimeoutError('Inspect original guest job ' + ident)

    def execute(self, session, expected_identity, expected_payload, expected_hashes):
        if not re.fullmatch(r'[A-Za-z0-9_.-]{1,95}', expected_payload): raise ValueError('invalid payload identity')
        paths = ('/media/system/EFI/BOOT/BOOTX64.EFI', '/media/system/KERNEL/BROOK.ELF', '/media/system/INITRD.IMG')
        if set(expected_hashes) != set(paths) or any(not re.fullmatch(r'[0-9a-f]{64}', expected_hashes[p]) for p in paths):
            raise ValueError('expected hashes for exactly the three original boot files')
        keys = ('port', 'sectors', 'logical_sector_bytes', 'physical_sector_bytes', 'model', 'serial')
        if set(expected_identity) != set(keys): raise ValueError('exact SSD identity required')
        if any(type(expected_identity[k]) is not int or expected_identity[k] < 0 for k in keys[:4]) or \
           any(not isinstance(expected_identity[k], str) or not expected_identity[k] for k in keys[4:]):
            raise ValueError('invalid SSD identity types')
        data = self.bridge.load_session(session)
        doctor = self.bridge.doctor(session)
        if doctor.get('probe_stage')!='verified' or doctor.get('reachable') is not True or doctor.get('identity_matches') is not True: raise RuntimeError('complete boot identity probe unavailable')
        state = doctor['guest']['writable_desktop']
        if state.get('ready') is not True or state.get('source_pinned') is not True or state.get('state_uncertain') is not False:
            raise RuntimeError('persistent desktop store is not safely admitted')
        actual = self.bridge.command(session, 'bridge hardware ahci-ro')
        if actual.get('ready') is not True or actual.get('quarantined') is not False or \
           any(type(actual.get(k)) is not type(expected_identity[k]) or actual.get(k) != expected_identity[k] for k in keys):
            raise RuntimeError('SSD identity or state mismatch')
        self.output.mkdir(parents=True, exist_ok=False, mode=0o700)
        self.save(state='prepared', session=session, guest_boot_id=data['guest_boot_id'], expected_payload=expected_payload,
                  expected_identity=expected_identity, expected_hashes=expected_hashes)
        try:
            ident = uuid.uuid4().hex; self.save(state='status_dispatched', status_job=ident)
            self.bridge.guest_run(session, ident, ['/boot/HWSTATUS.ELF'], cwd='/tmp', uid=0, wait_s=2)
            status = self.job(session, ident)
            if status.get('state') != 'completed' or status.get('exit', {}).get('code') != 0 or \
               'HARDWARE_PAYLOAD_ID ' + expected_payload not in status.get('stdout', {}).get('text', '').splitlines():
                raise RuntimeError('unexpected boot payload')
            self.save(status_job_result=status)
            ident = uuid.uuid4().hex; self.save(state='hashes_dispatched', hashes_job=ident)
            self.bridge.guest_run(session, ident, ['/nix/profile/bin/sha256sum', *paths], cwd='/tmp', uid=0, wait_s=2)
            hashes = self.job(session, ident)
            if hashes.get('state') != 'completed' or hashes.get('exit', {}).get('code') != 0:
                raise RuntimeError('boot-file hash command failed')
            lines = hashes.get('stdout', {}).get('text', '').splitlines()
            expected_lines = [expected_hashes[p] + '  ' + p for p in paths]
            if lines != expected_lines: raise RuntimeError('installed boot-file hashes do not match')
            self.save(hashes_job_result=hashes)
            ident = uuid.uuid4().hex
            self.save(state='mutation_dispatched', mutation_job=ident)
            self.bridge.guest_run(session, ident, ['/boot/SSDSLOT.ELF', expected_hashes[paths[0]]], cwd='/tmp', uid=0, wait_s=2)
            result = self.job(session, ident)
            self.save(job=result)
            if result.get('state') != 'completed' or result.get('exit', {}).get('code') != 0 or \
               'SSD_SLOT_SETUP_OK generation=2' not in result.get('stdout', {}).get('text', '').splitlines():
                raise RuntimeError('slot initializer did not report verified success')
            self.save(state='slots_initialized'); return self.receipt
        except Exception as error:
            previous = self.receipt.get('state')
            self.save(state='needs_inspection', last_stage=previous, error=str(error),
                      recovery='Inspect the original job and SSD state; do not retry a dispatched mutation automatically.')
            raise
