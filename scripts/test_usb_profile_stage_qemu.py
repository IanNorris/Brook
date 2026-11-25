#!/usr/bin/env python3
"""Prove signed profile staging and rollback on a disposable BROOK_SYSTEM."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import uuid

import rtx_smoke_update as rtx
import test_usb_system_qemu as fixture
from hardware import Hardware
from hardware_bulk_protocol import upload
from hardware_update_workflow import UpdateRun
from binary_bridge import receiver_key


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('files', 'store', 'operator-seed', 'output', 'plan', 'public-key'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--canary', default='brook-ota-canary')
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
        interrupted = upload(endpoint['host'], guest.bulk_port, receiver_key(endpoint, None),
                             endpoint['guest_boot_id'], tx, args.plan / 'stage-plan.json', finalize=False)
        assert interrupted == {'state': 'staged', 'published': False}
        stopped = run.wait_job(guest.session, tx, lambda status: status.get('state') == 'completed')
        assert stopped.get('exit', {}).get('code') != 0, stopped
        guest.job(f'[[ ! -e /data/.brook-transfer/{tx}.bin && ! -e /data/.brook-transfer/{tx}.part ]]')
        guest.restart()
    finally:
        guest.stop()
    check_protected()

    guest, bridge = boot('after-interruption')
    try:
        guest.job(old)
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

    guest, _ = boot('after-rollback')
    try:
        guest.job(old)
        guest.restart()
    finally:
        guest.stop()
    check_protected()
    result['outside_p2_unchanged'] = True
    (args.output / 'verdict.json').write_text(json.dumps(result, indent=2) + '\n')
    print(args.output / 'verdict.json')


if __name__ == '__main__':
    main()
