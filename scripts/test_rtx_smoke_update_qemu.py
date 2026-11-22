#!/usr/bin/env python3
"""QEMU proof of the RTX smoke stick's remote self-update (headless, KVM).

Boots a disposable copy of the v3 raw image (set A) in q35/OVMF with usb-net,
a second USB stick that carries an identical BROOK_RTX copy of set A (other
partition GUID), and writable AHCI/NVMe/virtio fixture disks. Scenarios:

  cycle     refusals first (unauthenticated operator key, bad manifest
            signature, tampered file: stick byte-identical afterwards), then
            push set B, hw_power restart, re-attach, check B's payload ID and
            the loader's trial state, confirm, check PREV/ holds A.
  fallback  push B, restart (trial boot of B), restart again without
            confirming: the loader must boot PREV/ (set A); confirm refused.
  label     same image with the FAT label changed to BROOK_RTY: the kernel
            gate refuses and the stick is unchanged.

Every scenario checks that the fixture disks and the second stick are
byte-identical at the end. The update key and operator seed are read by path
only (host side); this script never prints them.
"""
import argparse, hashlib, json, os, shutil, socket, subprocess, sys, tempfile, time, uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'scripts'), str(ROOT / 'tools'), str(ROOT / 'tools/vm_bridge')]
from test_rtx_smoke_qemu import Qmp, make_fixture_disks, sha  # noqa: E402
import build_rtx_smoke_files as builder  # noqa: E402
import rtx_smoke_update as tool  # noqa: E402


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def fat_files(image, names, offset=1 << 20):
    out = {}
    for name in names:
        try:
            data = subprocess.check_output(['mtype', '-i', f'{image}@@{offset}', '::/' + name], stderr=subprocess.DEVNULL)
            out[name] = hashlib.sha256(data).hexdigest()
        except subprocess.CalledProcessError:
            out[name] = None
    return out


class Vm:
    def __init__(self, base, stick, other, disks):
        self.base = base
        self.port, self.upload = free_port(), free_port()
        variables = base / 'VARS.fd'; shutil.copy2(os.environ['OVMF_VARS'], variables); variables.chmod(0o600)
        self.qmp_dir = Path(tempfile.mkdtemp(prefix='rtxu-')); self.qmp = self.qmp_dir / 'qmp.sock'
        self.serial = base / 'serial.log'
        argv = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm', '-cpu', 'host', '-smp', '4', '-m', '2048',
                '-nodefaults', '-drive', 'if=pflash,format=raw,readonly=on,file=' + os.environ['OVMF_CODE'],
                '-drive', 'if=pflash,format=raw,file=' + str(variables),
                '-device', 'VGA,id=video0', '-device', 'qemu-xhci,id=usbc',
                '-drive', f'if=none,id=stick,format=raw,file={stick}',
                '-device', 'usb-storage,bus=usbc.0,port=1,drive=stick,bootindex=1',
                '-drive', f'if=none,id=other,format=raw,file={other}',
                '-device', 'usb-storage,bus=usbc.0,port=3,drive=other',
                '-device', 'usb-net,bus=usbc.0,port=2,netdev=network',
                '-netdev', f'user,id=network,restrict=on,hostfwd=tcp:127.0.0.1:{self.port}-:1234,'
                           f'hostfwd=tcp:127.0.0.1:{self.upload}-:1235',
                '-device', 'ahci,id=sata', '-drive', f'if=none,id=ahci0,format=raw,file={disks["ahci"]}',
                '-device', 'ide-hd,bus=sata.0,drive=ahci0',
                '-drive', f'if=none,id=nvme0,format=raw,file={disks["nvme"]}', '-device', 'nvme,serial=brookupd,drive=nvme0',
                '-drive', f'if=none,id=vd0,format=raw,file={disks["virtio"]}', '-device', 'virtio-blk-pci,drive=vd0',
                '-display', 'none', '-serial', 'file:' + str(self.serial),
                '-qmp', 'unix:' + str(self.qmp) + ',server=on,wait=off']
        (base / 'argv.json').write_text(json.dumps(argv, indent=2) + '\n')
        self.log = (base / 'qemu.log').open('x')
        self.owner = subprocess.Popen(argv, stdout=self.log, stderr=subprocess.STDOUT)

    def text(self):
        return self.serial.read_text(errors='replace') if self.serial.exists() else ''

    def wait_for(self, needle, timeout=150, start=0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            assert self.owner.poll() is None, 'QEMU ended early\n' + self.text()[-3000:]
            text = self.text()
            assert 'PANIC_HEX:' not in text[start:], text[-3000:]
            index = text.find(needle, start)
            if index >= 0:
                return index
            time.sleep(0.2)
        raise TimeoutError(f'{needle!r} not seen\n' + self.text()[-3000:])

    def close(self):
        if self.owner.poll() is None:
            try: Qmp(self.qmp).call('quit')
            except Exception: self.owner.terminate()
        self.owner.wait(timeout=30); self.log.close()
        shutil.rmtree(self.qmp_dir, ignore_errors=True)


def unauthenticated(run, bridge, session, vm, blobs, manifest):
    """A valid signed set, sent with an operator key the stick does not list."""
    from nacl.signing import SigningKey
    data = bridge.load_session(session); boot = data['guest_boot_id']; tx = uuid.uuid4().hex
    bridge.guest_run(session, tx, ['/boot/HWRTXUP.ELF', boot, tx], cwd='/tmp', uid=0, wait_s=2)
    deadline = time.monotonic() + 30
    while 'HWUPDATE_LISTENING' not in bridge.job(session, tx, wait_s=1).get('stdout', {}).get('text', ''):
        assert time.monotonic() < deadline
    error = None
    try:
        tool.stream_set('127.0.0.1', vm.upload, SigningKey.generate(), boot, tx, manifest[0], manifest[1], blobs)
    except Exception as exc:  # the receiver drops the handshake
        error = type(exc).__name__ + ': ' + str(exc)
    while True:
        job = bridge.job(session, tx, wait_s=2)
        if job.get('state') in ('completed', 'failed', 'lost'):
            break
    return {'client_error': error, 'exit': job.get('exit', {}).get('code'),
            'stderr': job.get('stderr', {}).get('text', '').strip()}


def scenario(args, base, name):
    from hardware import Hardware
    base.mkdir(parents=True)
    stick = base / 'stick.raw'
    shutil.copyfile(args.image, stick)
    if name == 'label':
        with stick.open('r+b') as f:  # primary and backup FAT32 boot sectors
            for sector in (0, 6):
                f.seek((1 << 20) + sector * 512 + 0x47); f.write(b'BROOK_RTY  ')
    other = base / 'other.raw'
    builder.build_image(args.files_a, json.loads((args.files_a / 'manifest.json').read_text())['files'], other)
    disks = make_fixture_disks(base)
    keep = {**disks, 'other_stick': other}
    before = {k: sha(p) for k, p in keep.items()}
    stick_before = sha(stick)
    names = list(tool.NAMES)
    result = {'scenario': name}
    vm = Vm(base, stick, other, disks)
    bridge = Hardware(registry=base / 'hardware-registry')
    seed = str(args.operator_seed.absolute())
    try:
        vm.wait_for('DEBUG_TCP: listening on port 1234')
        vm.wait_for('BRIDGE endpoint tcp 10.0.2.15:1234', 90)
        text = vm.text()
        result['boot1_stick_line'] = [l for l in text.splitlines() if l.startswith('STICK:')][:3]
        session = bridge.attach('127.0.0.1', vm.port, auth_mode='v3', operator_key_file=seed)['session']
        run = tool.Run(bridge, None)
        result['boot1_status'] = run.status(session)
        assert result['boot1_status'].get('set') == 'root', result['boot1_status']
        if name == 'cycle':
            blobs, manifest = tool.load_set(args.files_b)
            binary = tool.binary_manifest(blobs, manifest['payload_id'], manifest['commit'])
            signature = tool.sign(tool.signing_key(args.key), binary)
            result['unauthenticated'] = unauthenticated(run, bridge, session, vm, blobs, (binary, signature))
            assert result['unauthenticated']['exit'] == 10, result['unauthenticated']
            assert sha(stick) == stick_before
            for tamper, code in (('signature', 41), ('file', 43)):
                receipt = tool.Run(bridge, base / f'refused-{tamper}').push(
                    session, args.files_b, args.key, vm.upload, False, False, 60, tamper=tamper)
                result[f'refused_{tamper}'] = {'state': receipt['state'], 'exit': receipt['job']['exit'],
                                              'stderr': receipt['job']['stderr']}
                assert receipt['state'] == 'refused' and receipt['job']['exit']['code'] == code, receipt['job']
                assert sha(stick) == stick_before, f'stick changed after {tamper} refusal'
            result['stick_unchanged_after_refusals'] = True
        if name == 'label':
            receipt = tool.Run(bridge, base / 'refused-label').push(session, args.files_b, args.key, vm.upload,
                                                                   False, False, 60)
            result['refused_label'] = {'state': receipt['state'], 'exit': receipt['job']['exit'],
                                       'stderr': receipt['job']['stderr'], 'reply': receipt['replies'][-1:]}
            assert receipt['state'] == 'refused' and receipt['job']['exit']['code'] == 23, receipt['job']
            assert 'code=5' in receipt['job']['stderr']  # BROOK_BOOT_STICK_REFUSED_LABEL
            assert 'STICK: update refused (volume is not FAT32 labelled BROOK_RTX)' in vm.text()
        if name in ('cycle', 'fallback'):
            receipt = tool.Run(bridge, base / 'push').push(session, args.files_b, args.key, vm.upload,
                                                          True, name == 'cycle', 240)
            result['push'] = {k: receipt.get(k) for k in ('state', 'update', 'new_payload_id', 'new_status',
                                                          'confirm', 'confirmed_status', 'new_build_id')}
            payload_b = json.loads((args.files_b / 'manifest.json').read_text())['payload_id']
            payload_a = json.loads((args.files_a / 'manifest.json').read_text())['payload_id']
            assert receipt['new_payload_id'] == payload_b, receipt
            if name == 'cycle':
                assert receipt['state'] == 'confirmed', receipt
                final = receipt['confirmed_status']
                assert final['set'] == 'root' and final['markers'] == '-' and final['prev_payload'] == payload_a, final
            else:
                assert receipt['state'] == 'new_boot_verified', receipt
                fresh = receipt['new_session']
                # Second restart without confirming: the trial is spent.
                second = tool.Run(bridge, base / 'second-restart')
                second.output.mkdir(mode=0o700)
                data = bridge.load_session(fresh)
                newest = second.restart(fresh, data, seed, data['guest_boot_id'], 240)
                result['fallback_payload'] = second.payload_id(newest['session'])
                result['fallback_status'] = second.status(newest['session'])
                assert result['fallback_payload'] == payload_a, result
                assert result['fallback_status']['set'] == 'PREV' and \
                    result['fallback_status']['trial'] == 'unconfirmed', result['fallback_status']
                code, text = second.confirm(newest['session'])
                result['confirm_on_prev'] = {'exit': code, 'stdout': text}
                assert code == 5 and 'HWRTXUP_CONFIRM_REFUSED' in text
            result['loader_lines'] = sorted({l.strip() for l in vm.text().splitlines() if 'Brook stick:' in l})
    finally:
        vm.close()
    after = {k: sha(p) for k, p in keep.items()}
    result['untouched'] = {k: before[k] == after[k] for k in keep}
    assert all(result['untouched'].values()), result['untouched']
    files = fat_files(stick, names + ['PREV/' + n.rsplit('/', 1)[-1] for n in names] + ['TRIAL.BIN', 'TRIAL.RUN'])
    result['stick_files'] = files
    want_b = {n: r['sha256'] for n, r in json.loads((args.files_b / 'manifest.json').read_text())['files'].items()}
    want_a = {n: r['sha256'] for n, r in json.loads((args.files_a / 'manifest.json').read_text())['files'].items()}
    if name == 'label':
        assert sha(stick) == stick_before, 'label scenario changed the stick'
        result['stick_unchanged'] = True
    else:
        for n, digest in want_b.items():
            assert files[n] == digest, (n, files[n])
        for n, digest in want_a.items():
            assert files['PREV/' + n.rsplit('/', 1)[-1]] == digest, ('PREV', n)
        assert files['TRIAL.BIN'] is None
        assert (files['TRIAL.RUN'] is None) == (name == 'cycle')
        result['root_is_b_prev_is_a'] = True
        # Independent oracle: dosfsck on the written partition.
        part = base / 'stick-part.img'
        with stick.open('rb') as src, part.open('wb') as dst:
            src.seek(1 << 20); shutil.copyfileobj(src, dst)
        fsck = subprocess.run(['fsck.fat', '-n', str(part)], capture_output=True, text=True)
        result['fsck_fat'] = {'exit': fsck.returncode, 'tail': fsck.stdout.strip().splitlines()[-2:]}
        part.unlink()
        assert fsck.returncode == 0, fsck.stdout
    (base / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    print('RTX_UPDATE_QEMU_PASS', name)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--files-a', type=Path, required=True); p.add_argument('--image', type=Path, required=True)
    p.add_argument('--files-b', type=Path, required=True); p.add_argument('--key', type=Path, required=True)
    p.add_argument('--operator-seed', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    p.add_argument('--scenario', action='append', choices=['cycle', 'fallback', 'label'])
    a = p.parse_args()
    if subprocess.run(['pgrep', '-x', 'vmware-vmx'], stdout=subprocess.DEVNULL).returncode == 0:
        raise SystemExit('VMware is running; QEMU proof skipped')
    a.output = a.output.absolute(); a.output.mkdir(parents=True, exist_ok=False)
    summary = {}
    for name in a.scenario or ['cycle', 'fallback', 'label']:
        summary[name] = scenario(a, a.output / name, name)
        (a.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print('RTX_UPDATE_QEMU_ALL_PASS')


if __name__ == '__main__':
    main()
