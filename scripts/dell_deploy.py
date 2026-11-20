#!/usr/bin/env python3
"""Deploy a pair built by build_dell_image.py to the Dell, only if it is fit to deploy.

Refuses unless <pair>/qemu-check/verdict.json says passed=true for this exact kernel/initrd, and the
build manifest is from a clean tree (override with --allow-dirty). Then runs
hardware_update_device.py --activate --restart with the payload ID taken from the manifest.
Use --profile-plan DIRECTORY for a matching signed stage plan and signature.
For a combined rollback, provide the previous checked --pair and the original
deployment's --rollback-profile receipt. Cache serving and on-device trust are
provisioned independently of deployment.

Usage (repo root, BROOK_ROOT set to the checkout holding the bridge session):
  python3 scripts/dell_deploy.py --session S --pair build/dell/pair-X --output build/dell/deploy-X
"""
import argparse, hashlib, json, subprocess, sys, tempfile, uuid
from contextlib import ExitStack
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KEY = Path('/data/Programming/Brook/shared/dell/hardware-update-key')

def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def stage_profile(args, *, bridge=None, bulk_port=1236):
    a = args
    sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tools/vm_bridge')]
    from hardware import Hardware
    from hardware_bulk_workflow import BulkRun
    from hardware_update_workflow import UpdateRun

    if bridge is None:
        bridge = Hardware()
    journal = UpdateRun(bridge, a.output / 'profile')
    journal.output.mkdir(mode=0o700, parents=True)
    tx = uuid.uuid4().hex
    guest = bridge.load_session(a.session)
    action = 'apply' if a.profile_plan else 'rollback'
    if a.rollback_profile:
        original = json.loads(a.rollback_profile.read_text())
        if original.get('action') != 'apply' or original.get('state') != 'profile_verified':
            raise ValueError('Expected the verified apply receipt from the original deployment')
        if original.get('endpoint') != [guest['host'], guest['port']]:
            raise ValueError('Rollback receipt belongs to a different device endpoint')
        tx = original['profile_transaction_id']
    files = ([a.profile_plan / name for name in ('stage-plan.json', 'stage-plan.sig')]
             if a.profile_plan else [])
    job = uuid.uuid4().hex
    journal.save(state='prepared', action=action, profile_transaction_id=tx,
                 profile_job_id=job, session=a.session, guest_boot_id=guest['guest_boot_id'],
                 endpoint=[guest['host'], guest['port']],
                 pair_manifest_sha256=sha(a.pair / 'build-manifest.json'),
                 profile_files={path.name: sha(path) for path in files})
    arguments = ['/nix/bin/nix', 'stage-profile', action, tx]
    try:
        with ExitStack() as resources:
            bundles = []
            if a.profile_plan:
                plan = json.loads(files[0].read_text())
                if a.push_profile_cache or plan['cache']['brook'] is None:
                    from profile_cache_bundle import pack_cache, validate_push_capacity
                    probe = uuid.uuid4().hex
                    journal.save(state='profile_cache_probe_dispatched', cache_probe_job_id=probe)
                    bridge.guest_run(a.session, probe, ['/nix/bin/nix', 'stage-profile', 'cache-capabilities'],
                                     cwd='/tmp', uid=1000, wait_s=2)
                    status = journal.wait_job(a.session, probe, lambda job: job.get('state') == 'completed',
                                              timeout=60)
                    journal.save(cache_probe_job=status)
                    if status.get('exit', {}).get('code') != 0:
                        raise ValueError('Guest stage helper lacks bridge cache capability; refresh it first')
                    capabilities = json.loads(status['stdout']['text'])
                    work = Path(resources.enter_context(tempfile.TemporaryDirectory(
                        prefix='.cache-bundles-', dir=journal.output)))
                    bundles = list(pack_cache(a.profile_plan / 'cache', plan, work))
                    required = validate_push_capacity([*files, *bundles], capabilities)
                    journal.save(state='profile_cache_preflight_verified', cache_required_bytes=required,
                                 cache_capabilities=capabilities)

            pushed = []
            for source in [*files, *bundles]:
                is_bundle = source in bundles
                digest = sha(source) if is_bundle else journal.receipt['profile_files'][source.name]
                bulk = BulkRun(bridge, journal.output / (source.stem if is_bundle else source.name))
                receipt = bulk.execute(a.session, KEY, 'put', source=source, port=bulk_port)
                transfer = receipt['transfer']
                if transfer['sha256'] != digest:
                    raise ValueError(f'{source.name} changed during transfer')
                if is_bundle:
                    pushed.append({'sha256': digest, 'guest_path': transfer['guest_path'],
                                   'receipt': str(bulk.output / 'receipt.json')})
                    journal.save(state='profile_cache_pushed', profile_cache_bundles=pushed)
                    arguments.append('--cache-bundle')
                    source.unlink()
                arguments.append(transfer['guest_path'])
        journal.save(state='profile_job_dispatched')
        bridge.guest_run(a.session, job, arguments, cwd='/tmp', uid=1000, wait_s=2)
        result = journal.wait_job(a.session, job, lambda status: status.get('state') == 'completed',
                                  timeout=a.profile_timeout_s)
        if result.get('exit', {}).get('code') != 0:
            raise RuntimeError(f'Profile job failed: {result}')
        journal.save(state='profile_verified', profile_job=result)
    except Exception as error:
        journal.save(state='needs_inspection', error=str(error))
        raise
    return journal


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--session', required=True); p.add_argument('--pair', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True); p.add_argument('--allow-dirty', action='store_true')
    p.add_argument('--boot-timeout-s', type=int, default=240)
    p.add_argument('--operator-key-file', type=Path, help='Host private key admitted by the new v3 image')
    p.add_argument('--profile-timeout-s', type=int, default=3600)
    profile = p.add_mutually_exclusive_group()
    profile.add_argument('--profile-plan', type=Path, metavar='DIRECTORY')
    profile.add_argument('--rollback-profile', type=Path, metavar='PROFILE_RECEIPT')
    p.add_argument('--push-profile-cache', action='store_true',
                   help='also push the local cache/ for a signed plan with a host URL')
    a = p.parse_args()
    if a.push_profile_cache and not a.profile_plan:
        p.error('--push-profile-cache requires --profile-plan')

    manifest = json.loads((a.pair / 'build-manifest.json').read_text())
    verdict_path = a.pair / 'qemu-check' / 'verdict.json'
    if not verdict_path.exists(): sys.exit(f'refusing: no QEMU verdict at {verdict_path}')
    verdict = json.loads(verdict_path.read_text())
    kernel, initrd = a.pair / 'kernel.elf', a.pair / 'INITRD.IMG'
    if verdict.get('passed') is not True: sys.exit('refusing: QEMU check did not pass (rerun scripts/check_dell_image_qemu.py --force)')
    for name, path in (('kernel_sha256', kernel), ('initrd_sha256', initrd)):
        if verdict.get(name) not in (None, sha(path)) or manifest.get(name) != sha(path):
            sys.exit(f'refusing: {name} does not match the checked/manifest pair')
    if manifest.get('dirty') and not a.allow_dirty: sys.exit('refusing: built from a dirty tree')
    next_v3 = []
    if manifest.get('bridge_v3') is not None:
        sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tools/vm_bridge')]
        from bridge_v3 import read_operator_key
        from bridge_operator_policy import validate_operator_policy
        from hardware import Hardware

        if (verdict.get('kernel_sha256'), verdict.get('initrd_sha256')) != (sha(kernel), sha(initrd)):
            sys.exit('refusing: v3 requires the exact checked kernel and initrd')
        policy_metadata = manifest['bridge_v3']
        if (policy_metadata.get('version'), policy_metadata.get('suite')) != (3, 1):
            sys.exit('refusing: unexpected protocol version or suite in the new image')
        try:
            session = Hardware().load_session(a.session)
            key_path = a.operator_key_file or session.get('operator_key_file')
            if not key_path:
                raise ValueError('The v3 image needs --operator-key-file for the first legacy cutover')
            path = Path(key_path).absolute()
            key = read_operator_key(path)
            policy = subprocess.check_output(['mtype', '-i', str(initrd), '::/ETC/bridge-v3-operators'])
            digest = hashlib.sha256(policy).hexdigest()
            verified = json.loads((a.pair / 'verify.json').read_text())
            if (digest != policy_metadata.get('policy_sha256') or
                    digest != verified.get('bridge_policy_sha256') or
                    verified.get('initrd_sha256') != sha(initrd) or
                    bytes(key.verify_key) not in validate_operator_policy(policy)):
                raise ValueError('The checked image policy does not admit the selected operator key')
            build_id = manifest['build_id']
        except (KeyError, OSError, ValueError, subprocess.SubprocessError) as error:
            sys.exit(f'refusing v3 cutover: {error}')
        next_v3 = ['--next-operator-key-file', str(path), '--next-operator-policy', digest,
                   '--next-build-id', build_id]
    elif a.operator_key_file:
        p.error('--operator-key-file only applies to a v3 image')
    stage = None
    kernel_output = a.output
    if a.profile_plan or a.rollback_profile:
        try:
            stage = stage_profile(a)
        except (OSError, RuntimeError, ValueError) as error:
            sys.exit(f'profile step failed: {error} (see {a.output / "profile/receipt.json"})')
        kernel_output = a.output / 'kernel'
    cmd = [sys.executable, str(ROOT / 'scripts/hardware_update_device.py'), '--session', a.session, '--key-file', str(KEY),
           '--kernel', str(kernel), '--initrd', str(initrd), '--output', str(kernel_output), '--activate', '--restart',
           '--expected-payload-id', manifest['payload_id'], '--helper', 'system', '--boot-timeout-s', str(a.boot_timeout_s)]
    code = subprocess.call(cmd + next_v3)
    receipt_path = kernel_output / 'receipt.json'
    if stage:
        stage.save(kernel_receipt=str(receipt_path.resolve()),
                   combined_state='needs_inspection' if code else 'boot_verified')
    if code and receipt_path.exists():
        # Name the stage and the guest processes still blocking a restart rather
        # than leaving a bare needs_inspection for the caller to dig out.
        sys.path.insert(0, str(ROOT / 'tools'))
        from hardware_update_workflow import power_blocker_summary
        receipt = json.loads(receipt_path.read_text())
        print(f"deploy: {receipt.get('state')} at {receipt.get('last_stage')}: {receipt.get('error')}", file=sys.stderr)
        if 'power' in receipt:
            print(f"deploy: restart request {receipt.get('power_request_id')}: {power_blocker_summary(receipt['power'])}", file=sys.stderr)
    return code


if __name__ == '__main__':
    sys.exit(main())
