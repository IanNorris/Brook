#!/usr/bin/env python3
"""Re-select an older, verified SSD boot slot using the attached boot's transport.

The booted system updater must support running-slot protection. The exact
kernel and initrd must match the older selector. An uncertain result is
inspected through the original job and receipt, without dispatching it again.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tools/vm_bridge')]
from binary_bridge import receiver_key
from hardware import Hardware
from hardware_update_protocol import INITRD_MAX, KERNEL_MAX, promote, read_payload
from hardware_update_workflow import UpdateRun


def run(bridge, session, key_file, slot, kernel_path, initrd_path, output,
        *, port=1235, expected_generation=None, activate=False):
    data = bridge.load_session(session)
    key = receiver_key(data, key_file)
    boot = data['guest_boot_id']
    doctor = bridge.doctor(session)
    if (doctor.get('probe_stage') != 'verified' or not doctor.get('reachable') or
            not doctor.get('identity_matches')):
        raise RuntimeError('boot probe is incomplete or does not match the pinned session')
    kernel = read_payload(kernel_path, KERNEL_MAX)
    initrd = read_payload(initrd_path, INITRD_MAX)
    operation = UpdateRun(bridge, output)
    operation.output.mkdir(parents=True, exist_ok=False, mode=0o700)
    tx = uuid.uuid4().hex
    operation.save(state='prepared', session=session, boot_id=boot, transaction_id=tx, slot=slot,
                   auth_mode=data['auth_mode'], expected_generation=expected_generation, activate=activate,
                   kernel_sha256=hashlib.sha256(kernel).hexdigest(),
                   initrd_sha256=hashlib.sha256(initrd).hexdigest())
    try:
        operation.save(state='helper_dispatching')
        job = bridge.guest_run(session, tx, ['/boot/HWUPSYS.ELF', boot, tx, '--require-slot-aware'],
                               cwd='/tmp', uid=0, wait_s=2)
        operation.save(state='helper_dispatched', job_initial=job)

        def listening(reply):
            if reply.get('exit', {}).get('code') == 2:
                raise RuntimeError('The booted system receiver lacks running-slot support. '
                                   'Use maintenance to stage a matching updater before retrying.')
            return 'HWUPDATE_LISTENING' in reply.get('stdout', {}).get('text', '')

        if not listening(job):
            operation.wait_job(session, tx, listening)
        operation.save(state='helper_listening')
        result = promote(data['host'], port, key, boot, tx, slot, kernel, initrd, activate,
                         expected_generation=expected_generation)
        operation.save(state='promote_reply', result=result)
        if job.get('state') != 'completed':
            job = operation.wait_job(session, tx, lambda reply: reply.get('state') == 'completed')
        operation.save(job_final=job)
        if activate:
            marker = re.search(r'HWUPDATE_PROMOTED generation=(\d+) slot=([01]) transaction=([0-9a-f]{32})\b',
                               job.get('stdout', {}).get('text', ''))
            if (job.get('exit', {}).get('code') != 0 or not marker or
                    (int(marker[1]), int(marker[2]), marker[3]) != (result['generation'], slot, tx)):
                raise RuntimeError('Promotion job and binary reply disagree')
        elif job.get('exit', {}).get('code') != 22:
            raise RuntimeError('Unexpected staged-only promotion completion')
        operation.save(state=result['state'])
        return operation.receipt
    except Exception as error:
        operation.save(state='needs_inspection', last_stage=operation.receipt['state'], error=str(error))
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', required=True)
    parser.add_argument('--key-file', type=Path, help='credential for an explicit legacy session')
    parser.add_argument('--slot', type=int, choices=(0, 1), required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--initrd', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expected-generation', type=int)
    parser.add_argument('--port', type=int, default=1235)
    parser.add_argument('--activate', action='store_true')
    args = parser.parse_args(argv)
    receipt = run(Hardware(), args.session, args.key_file, args.slot, args.kernel, args.initrd,
                  args.output, port=args.port, expected_generation=args.expected_generation,
                  activate=args.activate)
    print(json.dumps({key: receipt[key] for key in ('state', 'transaction_id', 'result')}, indent=2))


if __name__ == '__main__':
    main()
