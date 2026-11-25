#!/usr/bin/env python3
"""Prove signed profile staging and rollback on a disposable BROOK_SYSTEM."""
import argparse
import errno
import hashlib
import json
from pathlib import Path
import re
import subprocess
import socket
import struct
import time
import uuid

import rtx_smoke_update as rtx
import test_usb_system_qemu as fixture
from hardware import Hardware
from hardware_bulk_protocol import authenticate, line, upload
from hardware_bulk_workflow import BulkRun
from profile_cache_bundle import pack_cache
from hardware_update_workflow import UpdateRun
from binary_bridge import receiver_key


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('files', 'store', 'operator-seed', 'output', 'plan', 'public-key'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--canary', default='brook-ota-canary')
    parser.add_argument('--expect-refusal', choices=('space', 'bind'))
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.canary):
        parser.error('Expected a basename for the profile canary')
    if args.accel == 'kvm' and subprocess.run(['pgrep', '-x', 'vmware-vmx'], capture_output=True).returncode == 0:
        parser.error('Workstation is running. Use TCG or wait for the VMware window to close.')
    args.profile_transfer = True
    args.boot_set_manifest = args.files / 'manifest.json'
    args.repair = []
    args.fixed_port = fixture.available_port()
    args.output.mkdir(mode=0o700, parents=True)
    fixture.adopt_prebuilt(args.files)
    system, fallback = fixture.make_disks(args)
    outside = fixture.digest_outside_system(system)
    backup = fixture.sha(fallback)
    result = {}

    def check_protected():
        assert fixture.digest_outside_system(system) == outside
        assert fixture.sha(fallback) == backup

    def boot(case):
        guest = fixture.Guest(args, system, fallback, case)
        try:
            guest.wait_log('USB_SYSTEM_READY')
            identity = guest.attach()
            result[case] = identity['boot_id']
            return guest, Hardware(guest.base / 'hardware-registry')
        except BaseException:
            guest.stop()
            raise

    def submit(guest, bridge, argv, uid=1000):
        job = uuid.uuid4().hex
        bridge.guest_run(guest.session, job, argv, cwd='/tmp', uid=uid, wait_s=2)
        return job

    def wait(guest, bridge, job, predicate=None):
        return UpdateRun(bridge, args.output).wait_job(
            guest.session, job, predicate or (lambda row: row.get('state') == 'completed'),
            timeout=120)

    def push(guest, bridge, path):
        transfer = BulkRun(bridge, guest.base / uuid.uuid4().hex).execute(
            guest.session, None, 'put', source=path, port=guest.bulk_port)['transfer']
        assert transfer['retention'] == 'persistent_system', transfer
        return transfer['guest_path']

    def verify(guest, bridge, document, signature, complaint):
        paths = [push(guest, bridge, path) for path in (document, signature)]
        argv = ['/boot/bin/bash', '/boot/USBSTAGE.RC', 'verify', uuid.uuid4().hex, *paths]
        job = submit(guest, bridge, argv)
        answer = wait(guest, bridge, job)
        assert answer['exit']['code'] != 0 and complaint in answer['stderr']['text'], answer

    def finish():
        check_protected()
        result['outside_p2_unchanged'] = True
        (args.output / 'verdict.json').write_text(json.dumps(result, indent=2) + '\n')
        print(args.output / 'verdict.json')

    if args.expect_refusal:
        guest, bridge = boot('refusal')
        try:
            if args.expect_refusal == 'bind':
                assert not guest.status()['profile_transfer_ready']
                try:
                    rtx.Run(bridge, args.output / 'denied').profile_target(guest.session)
                except RuntimeError as error:
                    assert 'transfer bind' in str(error)
                else:
                    raise AssertionError('Updater accepted the refused transfer bind')
            else:
                endpoint = bridge.load_session(guest.session)
                tx = uuid.uuid4().hex
                argv = ['/boot/HWBULK.ELF', endpoint['guest_boot_id'], tx, 'put']
                bridge.guest_run(guest.session, tx, argv, cwd='/tmp', uid=0, wait_s=2)
                wait(guest, bridge, tx, lambda job: 'HWBULK_LISTENING' in job.get('stdout', {}).get('text', ''))
                try:
                    upload(endpoint['host'], guest.bulk_port, receiver_key(endpoint, None),
                           endpoint['guest_boot_id'], tx, args.plan / 'stage-plan.sig')
                except RuntimeError as error:
                    assert error.args[0]['errno'] == errno.ENOSPC, error
                    reply = wait(guest, bridge, tx)
                    assert reply['exit']['code'] == 27, reply
                else:
                    raise AssertionError('Receiver consumed its storage reserve')
            result[args.expect_refusal + '_refused'] = True
            guest.restart()
        finally:
            guest.stop()
        finish()
        return

    current = '/nix/var/.brook-profiles/current'
    package = f'/nix/profile/bin/{args.canary}'
    old = f'[[ ! -e {package} ]]; [[ $(readlink {current}) == .brook-profile-legacy ]]'
    guest, bridge = boot('initial')
    try:
        init = rtx.Run(bridge, args.output / 'bootstrap').profile_bootstrap(guest.session)
        assert init['state'] == 'profile_initialized', init
        again = rtx.Run(bridge, args.output / 'bootstrap-again').profile_bootstrap(guest.session)
        assert again['state'] == 'profile_initialized', again
        guest.job(old)
        tx = uuid.uuid4().hex
        endpoint = bridge.load_session(guest.session)
        bridge.guest_run(guest.session, tx, ['/boot/HWBULK.ELF', endpoint['guest_boot_id'], tx, 'put'],
                         cwd='/tmp', uid=0, wait_s=2)
        run = UpdateRun(bridge, args.output)
        run.wait_job(guest.session, tx,
                     lambda status: 'HWBULK_LISTENING' in status.get('stdout', {}).get('text', ''))
        content = (args.plan / 'stage-plan.json').read_bytes()
        with socket.create_connection((endpoint['host'], guest.bulk_port), timeout=30) as raw:
            stream = authenticate(raw, receiver_key(endpoint, None), endpoint['guest_boot_id'], tx, 'put')
            stream.sendall(struct.pack('<Q', len(content)) + hashlib.sha256(content).digest() + content)
            assert line(stream) == {'state': 'staged', 'published': False}
            receiver_pid = bridge.job(guest.session, tx)['guest']['pid']
            assert receiver_pid > 1
            kill = submit(guest, bridge, ['/boot/bin/bash', '-c', f'kill -KILL {receiver_pid}'], uid=0)
            assert wait(guest, bridge, kill)['exit']['code'] == 0
            stopped = wait(guest, bridge, tx)
            assert stopped['exit']['signal'] == 9, stopped
        guest.job(f'[[ ! -e /data/.brook-transfer/{tx}.bin && -e /data/.brook-transfer/{tx}.part ]]')
        result['put_sigkill'] = True
        guest.restart()
    finally:
        guest.stop()
    check_protected()

    guest, bridge = boot('after-interruption')
    try:
        guest.job(old)
        guest.job(f'[[ ! -e /data/.brook-transfer/{tx}.part ]]')
        document = args.plan / 'stage-plan.json'
        signature = args.plan / 'stage-plan.sig'
        altered = args.output / 'tampered.json'
        altered.write_bytes(document.read_bytes() + b' ')
        verify(guest, bridge, altered, signature, 'pkeyutl')
        wrong_key = args.output / 'wrong-test-key.pem'
        wrong_signature = args.output / 'wrong.sig'
        subprocess.run(['openssl', 'genpkey', '-algorithm', 'ED25519', '-out', wrong_key], check=True)
        subprocess.run(['openssl', 'pkeyutl', '-sign', '-rawin', '-inkey', wrong_key, '-in', document,
                        '-out', wrong_signature], check=True)
        wrong_key.unlink()
        verify(guest, bridge, document, wrong_signature, 'pkeyutl')
        result['tampered_wrong_key_refused'] = True

        plan = json.loads(document.read_text())
        local_cache = args.plan / 'cache'
        available = {name for name in plan['closure'] if (local_cache / (name[:32] + '.narinfo')).exists()}
        cache = list(pack_cache(local_cache, plan, guest.base, missing=available))
        apply = submit(guest, bridge, ['/boot/bin/bash', '/boot/USBSTAGE.RC', 'apply',
                       uuid.uuid4().hex, push(guest, bridge, document), push(guest, bridge, signature),
                       '--cache-bundle', push(guest, bridge, cache[0])])
        deadline = time.monotonic() + 90
        while True:
            status = submit(guest, bridge, ['/boot/bin/bash', '/boot/USBSTAGE.RC', 'status'])
            reply = wait(guest, bridge, status)
            if reply['exit']['code'] == 0 and json.loads(reply['stdout']['text'])['active']:
                break
            assert time.monotonic() < deadline, reply
        try:
            endpoint = bridge.load_session(guest.session)
            rtx.Run(bridge, None).restart(guest.session, endpoint, str(args.operator_seed),
                                         endpoint['guest_boot_id'], 30)
        except RuntimeError as error:
            assert 'restart refused' in str(error)
        else:
            raise AssertionError('Host allowed restart with active staging')
        pid = bridge.job(guest.session, apply)['guest']['pid']
        assert pid > 1
        kill = submit(guest, bridge, ['/boot/bin/bash', '-c', f'kill -KILL {pid}'], uid=0)
        assert wait(guest, bridge, kill)['exit']['code'] == 0
        assert wait(guest, bridge, apply)['exit']['signal'] == 9
        result['apply_sigkill'] = True
        guest.restart()
    finally:
        guest.stop()
    check_protected()

    guest, bridge = boot('after-killed-apply')
    try:
        guest.job(old)
        recover = submit(guest, bridge, ['/boot/bin/bash', '/boot/USBSTAGE.RC', 'recover'], uid=0)
        recovered = wait(guest, bridge, recover)
        assert recovered['exit']['code'] == 0, recovered
        guest.job('[[ ! -e /nix/var/.brook-profile-lock ]]')
        result['stale_lock_recovered'] = True
        args.session = guest.session
        args.bulk_port = guest.bulk_port
        args.profile_plan = args.plan
        args.rollback_profile = None
        args.profile_timeout_s = 480
        operator = rtx.Run(bridge, args.output / 'apply')
        receipt = operator.profile(argparse.Namespace(**{**vars(args), 'output': operator.output}))
        assert receipt['state'] == 'profile_verified' and receipt['missing_store_paths'], receipt
        result['missing_store_paths'] = receipt['missing_store_paths']
        transaction = receipt['profile_transaction_id']
        guest.restart()
    finally:
        guest.stop()
    check_protected()

    guest, bridge = boot('after-apply')
    try:
        guest.job(f'[[ -x {package} && $(readlink {current}) == .brook-profile-{transaction} ]]')
        args.session = guest.session
        args.bulk_port = guest.bulk_port
        args.profile_plan = args.public_key = None
        args.rollback_profile = args.output / 'apply/profile/receipt.json'
        operator = rtx.Run(bridge, args.output / 'rollback')
        receipt = operator.profile(argparse.Namespace(**{**vars(args), 'output': operator.output}))
        assert receipt['state'] == 'profile_verified', receipt
        guest.restart()
    finally:
        guest.stop()
    check_protected()

    guest, bridge = boot('after-rollback')
    try:
        guest.job(old)
        verify(guest, bridge, args.plan / 'stage-plan.json', args.plan / 'stage-plan.sig',
               'high-water mark')
        result['replay_refused_after_rollback'] = True
        guest.restart()
    finally:
        guest.stop()
    finish()


if __name__ == '__main__':
    main()
