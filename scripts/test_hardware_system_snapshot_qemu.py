#!/usr/bin/env python3
"""Exercise the production SSD-store snapshot helper on private QEMU media.

The source must be a stopped regular-file Dell-capacity QEMU fixture. A
reflink clone is placed in maintenance mode by removing its store admission
record from the clone's initrd; no physical device or user image is modified.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tools/vm_bridge'), str(ROOT / 'scripts')]
from hardware import Hardware
from hardware_bulk_protocol import download
from hardware_bulk_workflow import BulkRun
from test_hardware_power import Qmp


def run(*args):
    subprocess.run([str(x) for x in args], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)


def sha(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('fixture', 'snapshot-helper', 'store', 'key-file', 'output'):
        parser.add_argument('--' + option, required=True, type=Path)
    parser.add_argument('--dirty-classification-kernel', type=Path,
                        help='Instead verify that this kernel refuses a deliberately unclean store without changing it')
    parser.add_argument('--readonly-checker', type=Path,
                        help='Also run a statically linked e2fsck -fn on the private dirty image')
    parser.add_argument('--preen-checker', type=Path,
                        help='Run e2fsck -p on the disposable dirty image and verify it offline')
    parser.add_argument('--chmod-bootstrap', type=Path,
                        help='Test staging a RAM-only chmod helper through the recovery shell')
    parser.add_argument('--recovery-initrd', type=Path,
                        help='Copy the production recovery shell into the disposable initrd')
    parser.add_argument('--network-roundtrip', action='store_true',
                        help='Also verify a binary RAM-only upload and download')
    parser.add_argument('--xhci-module', type=Path,
                        help='Use this xHCI module in the private QEMU boot')
    parser.add_argument('--candidate-initrd', type=Path,
                        help='Use an already-prepared recovery initrd unchanged')
    parser.add_argument('--expect-identity-refusal', action='store_true',
                        help='Expect a physical-device-bound initrd to reject the private QEMU disk')
    a = parser.parse_args()
    if (a.readonly_checker or a.preen_checker) and not a.dirty_classification_kernel:
        parser.error('checker tests require --dirty-classification-kernel')
    if a.readonly_checker and a.preen_checker:
        parser.error('choose one checker mode')
    if a.candidate_initrd and (not a.dirty_classification_kernel or a.xhci_module or a.recovery_initrd or a.readonly_checker or a.preen_checker):
        parser.error('candidate initrd requires dirty mode and no initrd modifications')
    if a.expect_identity_refusal and not a.candidate_initrd:
        parser.error('identity refusal requires a candidate initrd')
    if a.fixture.is_symlink() or not a.fixture.is_file() or a.fixture.stat().st_size != 500118192 * 512:
        raise ValueError('expected a stopped Dell-capacity regular-file QEMU fixture')
    out = a.output.absolute(); out.mkdir(parents=True, exist_ok=False, mode=0o700)
    disk = out / 'sata.raw'; run('cp', '--reflink=always', a.fixture, disk)
    media = str(disk) + '@@1048576'; initrd = out / 'initrd.img'
    if a.candidate_initrd: shutil.copyfile(a.candidate_initrd, initrd)
    else: run('mcopy', '-i', media, '::INITRD.IMG', initrd)
    dirty_store = None
    if a.dirty_classification_kernel:
        dirty_store = out / 'dirty-store.img'
        run('cp', '--reflink=always', a.store, dirty_store)
        with dirty_store.open('r+b') as f:
            f.seek(1082); f.write(b'\0\0'); f.flush(); os.fsync(f.fileno())
        run('mcopy', '-o', '-i', media, dirty_store, '::STORE.IMG')
        run('mcopy', '-o', '-i', media, a.dirty_classification_kernel, '::KERNEL/BROOK.ELF')
    else:
        run('mdel', '-i', initrd, '::ETC/writable-desktop')
    if not a.candidate_initrd:
        run('mcopy', '-i', initrd, a.snapshot_helper, '::BOOT/HWSNAP.ELF')
    if a.xhci_module:
        run('mcopy', '-o', '-i', initrd, a.xhci_module, '::DRIVERS/XHCI.MOD')
    if a.recovery_initrd:
        shell = out / 'busybox'
        run('mcopy', '-i', a.recovery_initrd, '::BOOT/BUSYBOX', shell)
        run('mcopy', '-o', '-i', initrd, shell, '::BOOT/BUSYBOX')
    if a.readonly_checker or a.preen_checker:
        run('mcopy', '-i', initrd, a.readonly_checker or a.preen_checker, '::BOOT/E2FSCK.ELF')
    run('mcopy', '-o', '-i', media, initrd, '::INITRD.IMG')
    variables = out / 'VARS.fd'; shutil.copy2(os.environ['OVMF_VARS'], variables); variables.chmod(0o600)
    ports = []
    for _ in range(2):
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0)); ports.append(s.getsockname()[1])
    argv = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm', '-cpu', 'host', '-smp', '4', '-m', '4096', '-nodefaults',
            '-drive', 'if=pflash,format=raw,readonly=on,file=' + os.environ['OVMF_CODE'],
            '-drive', 'if=pflash,format=raw,file=' + str(variables), '-device', 'VGA,vgamem_mb=64',
            '-drive', f'if=none,id=sata,format=raw,file={disk}',
            '-device', 'ide-hd,drive=sata,bus=ide.0,bootindex=1,serial=BROOK-SYSTEM-TEST01',
            '-device', 'qemu-xhci,id=usbc', '-device', 'usb-net,bus=usbc.0,port=2,netdev=network',
            '-netdev', f'user,id=network,restrict=on,hostfwd=tcp:127.0.0.1:{ports[0]}-:1234,hostfwd=tcp:127.0.0.1:{ports[1]}-:1236',
            '-display', 'none', '-serial', 'file:' + str(out / 'serial.log'),
            '-qmp', 'unix:' + str(out / 'qmp.sock') + ',server=on,wait=off', '-no-reboot']
    (out / 'argv.json').write_text(json.dumps(argv, indent=2) + '\n')
    with (out / 'qemu.log').open('x') as log:
        owner = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        qmp = None
        try:
            deadline = time.monotonic() + 360
            while time.monotonic() < deadline:
                if owner.poll() is not None:
                    raise RuntimeError('QEMU exited during maintenance boot')
                serial = (out / 'serial.log').read_text(errors='replace') if (out / 'serial.log').exists() else ''
                if 'PANIC_HEX:' in serial:
                    raise RuntimeError('guest panic during maintenance boot')
                if 'HARDWARE_NETWORK_READY' in serial and 'DEBUG_TCP: listening' in serial:
                    break
                time.sleep(.2)
            else:
                raise TimeoutError('maintenance boot did not reach debug interface')
            h = Hardware(out / 'registry'); attached = h.attach('127.0.0.1', ports[0]); s = attached['session']
            caps = h.command(s, 'bridge hardware caps')
            assert not caps['writable_desktop']['ready'] and not caps['writable_desktop']['source_pinned']
            assert 'writable desktop: ready' not in serial
            if a.expect_identity_refusal:
                assert 'ahci system: disk identity or writable state failed' in serial
            else:
                assert 'ahci system: verified GPT entry' in serial
            if dirty_store:
                if not a.expect_identity_refusal:
                    assert caps['writable_desktop']['stage'] == 'needs-filesystem-check'
                    assert caps['writable_desktop']['error'] == -117
                if a.network_roundtrip:
                    payload = out / 'network-payload.bin'
                    payload.write_bytes(bytes(range(251)) * 8193)
                    transfer = BulkRun(h, out / 'network-upload').execute(s, a.key_file, 'put', source=payload, port=ports[1])
                    received = BulkRun(h, out / 'network-download').execute(s, a.key_file, 'get',
                        artifact_id=transfer['transaction_id'], port=ports[1])
                    assert sha(payload) == sha(Path(received['artifact_path']))
                if a.chmod_bootstrap:
                    payload = a.chmod_bootstrap.read_bytes()
                    for offset in range(0, len(payload), 2048):
                        encoded = ''.join('\\%03o' % byte for byte in payload[offset:offset + 2048])
                        redirect = '>' if offset == 0 else '>>'
                        script = "printf '%b' '" + encoded + "' " + redirect + ' /boot/STOREQA.ELF'
                        ident = uuid.uuid4().hex
                        h.guest_run(s, ident, ['/boot/busybox', 'sh', '-c', script], cwd='/tmp', uid=0, wait_s=2)
                        job = h.job(s, ident, wait_s=20)
                        if job['state'] != 'completed' or job.get('exit', {}).get('code') != 0:
                            raise RuntimeError('bootstrap staging failed: ' + repr(job))
                    for argv in (['/boot/busybox', 'sh', '-c', 'printf test > /tmp/chmod-probe'],
                                 ['/boot/STOREQA.ELF', '/tmp/chmod-probe'],
                                 ['/boot/busybox', 'sh', '-c', 'test -x /tmp/chmod-probe']):
                        ident = uuid.uuid4().hex
                        h.guest_run(s, ident, argv, cwd='/tmp', uid=0, wait_s=2)
                        job = h.job(s, ident, wait_s=20)
                        if job['state'] != 'completed' or job.get('exit', {}).get('code') != 0:
                            raise RuntimeError('bootstrap verification failed: ' + repr(job))
                if a.readonly_checker or a.preen_checker:
                    checker_id = uuid.uuid4().hex
                    checker_args = ['-f', '-n'] if a.readonly_checker else ['-p']
                    h.guest_run(s, checker_id, ['/boot/E2FSCK.ELF', *checker_args, '/media/system/STORE.IMG'],
                                cwd='/tmp', uid=0, wait_s=2)
                    deadline = time.monotonic() + 240
                    while True:
                        job = h.job(s, checker_id, wait_s=2)
                        (out / 'checker-job.json').write_text(json.dumps(job, indent=2) + '\n')
                        if job['state'] in ('completed', 'failed', 'lost', 'rejected'):
                            break
                        if time.monotonic() >= deadline:
                            raise TimeoutError('checker did not finish; inspect original job ' + checker_id)
                    acceptable = (0,) if a.readonly_checker else (0, 1)
                    if job['state'] != 'completed' or job.get('exit', {}).get('code') not in acceptable:
                        raise RuntimeError('checker failed; see checker-job.json')
                request = uuid.uuid4().hex; state = h.power(s, request, 'shutdown'); deadline = time.monotonic() + 120
                while state['state'] != 'ready':
                    if state['state'] in ('failed', 'unsupported') or time.monotonic() >= deadline:
                        raise RuntimeError('shutdown did not reach ready')
                    time.sleep(.2); state = h.power(s, request, 'status')
                qmp = Qmp(out / 'qmp.sock'); qmp.call('quit'); assert owner.wait(timeout=15) == 0
                recovered = out / 'after-store.img'; run('mcopy', '-i', media, '::STORE.IMG', recovered)
                if a.preen_checker:
                    # Host verification is deliberately read-only and occurs after
                    # QEMU has shut down, on the extracted disposable image.
                    check = subprocess.run([str(a.preen_checker), '-f', '-n', str(recovered)],
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                    (out / 'host-check.log').write_bytes(check.stdout)
                    if check.returncode != 0:
                        raise RuntimeError('offline host verification failed; see host-check.log')
                    with recovered.open('rb') as f:
                        f.seek(1082)
                        assert f.read(2) == b'\x01\x00', 'preen did not mark image clean'
                else:
                    assert sha(recovered) == sha(dirty_store)
                (out / 'result.json').write_text(json.dumps({'passed': True, 'stage': 'identity-refused' if a.expect_identity_refusal else 'needs-filesystem-check',
                    'store_unmounted': True, 'store_bytes_unchanged': not bool(a.preen_checker),
                    'checked_shutdown': True, 'readonly_checker_passed': bool(a.readonly_checker),
                    'disposable_preen_passed': bool(a.preen_checker),
                    'chmod_bootstrap_passed': bool(a.chmod_bootstrap),
                    'network_roundtrip_passed': bool(a.network_roundtrip)}, indent=2) + '\n')
                print('DIRTY_STORE_CLASSIFICATION_QEMU_PASS', flush=True)
                return
            tx = uuid.uuid4().hex
            h.guest_run(s, tx, ['/boot/HWSNAP.ELF', attached['guest_boot_id'], tx, 'get', uuid.uuid4().hex],
                        cwd='/tmp', uid=0, wait_s=2)
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                job = h.job(s, tx, wait_s=2)
                if 'HWBULK_LISTENING' in job.get('stdout', {}).get('text', ''):
                    break
                if job['state'] in ('completed', 'failed', 'lost', 'rejected'):
                    raise RuntimeError('snapshot helper did not listen')
            else:
                raise TimeoutError('inspect original snapshot job ' + tx)
            key = a.key_file.read_text()[:32]
            result = download('127.0.0.1', ports[1], key, attached['guest_boot_id'], tx,
                              out / 'snapshot.img', max_bytes=4026531840, timeout=3600)
            job = h.job(s, tx, wait_s=20)
            assert job.get('exit', {}).get('code') == 0
            assert result['guest_path'] == '/media/system/STORE.IMG'
            assert sha(out / 'snapshot.img') == result['sha256'] == sha(a.store)
            request = uuid.uuid4().hex; state = h.power(s, request, 'shutdown'); deadline = time.monotonic() + 120
            while state['state'] != 'ready':
                if state['state'] in ('failed', 'unsupported') or time.monotonic() >= deadline:
                    raise RuntimeError('shutdown did not reach ready')
                time.sleep(.2); state = h.power(s, request, 'status')
            qmp = Qmp(out / 'qmp.sock'); qmp.call('quit'); assert owner.wait(timeout=15) == 0
            (out / 'result.json').write_text(json.dumps({'passed': True, 'store_mounted': False,
                'independent_readback_sha256': result['sha256'], 'checked_shutdown': True}, indent=2) + '\n')
            print('SYSTEM_STORE_SNAPSHOT_QEMU_PASS', flush=True)
        finally:
            if qmp:
                qmp.close()
            if owner.poll() is None:
                owner.terminate(); owner.wait(timeout=15)


if __name__ == '__main__':
    main()
