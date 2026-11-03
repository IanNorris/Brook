#!/usr/bin/env python3
"""Prepare, but do not install, a RAM-resident SSD recovery kernel/initrd pair.

The source is an already reviewed physical maintenance pair. The resulting
image keeps its identity-bound writable SSD access but removes desktop-store
mounts and applications. It provides a static checker for an operator; this
preparer does not automatically repair, reboot, or write any physical disk.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from hardware_update_protocol import INITRD_MAX, KERNEL_MAX, record
from hardware_recovery_fsck import embed_checker

INIT = (b'set wm\nset vfb none\nsched eevdf\nkconsole\n'
        b'run --console --uid 1000 /boot/HWSTATUS.ELF\n')
PAYLOAD_ID = b'brook-ssd-recovery-manual-v1\n'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_regular(path, maximum):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise ValueError(f'{path}: expected a regular file')
        with os.fdopen(fd, 'rb', closefd=False) as stream:
            data = stream.read(maximum + 1)
        if len(data) > maximum:
            raise ValueError(f'{path}: exceeds size limit')
        return data
    finally:
        os.close(fd)


def prepare(source, checker, validator, out):
    manifest_data = read_regular(source / 'manifest.json', 1024 * 1024)
    manifest = json.loads(manifest_data)
    inputs = {}
    for name, maximum in (('kernel.elf', KERNEL_MAX), ('INITRD.IMG', INITRD_MAX)):
        data = read_regular(source / name, maximum)
        expected = manifest['files'][name]
        if len(data) != expected['bytes'] or sha(data) != expected['sha256']:
            raise ValueError(f'{name}: source manifest mismatch')
        inputs[name] = data
    subprocess.run([str(validator)], input=inputs['kernel.elf'], check=True)
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    try:
        (out / 'KERNEL.ELF').write_bytes(inputs['kernel.elf'])
        image = out / 'INITRD.IMG'
        image.write_bytes(inputs['INITRD.IMG'])
        def fat(path):
            return subprocess.run(['mtype', '-i', str(image), '::' + path],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        policy = {}
        for name in ('ssd-block-tools', 'ssd-install-mode', 'ahci-write-target'):
            read = fat('ETC/' + name)
            if read.returncode or not read.stdout:
                raise ValueError(f'missing maintenance policy: {name}')
            policy[name] = read.stdout
        if policy['ssd-block-tools'] != b'1\n' or policy['ssd-install-mode'] != b'1\n':
            raise ValueError('maintenance policy markers are not enabled')
        for name in ('ahci-boot-volume', 'ahci-data-volume', 'boot-app-image'):
            if fat('ETC/' + name).returncode == 0:
                raise ValueError(f'conflicting boot mount selector: {name}')
        # Space on a 32 MiB initrd is limited; a wallpaper is not needed for
        # the console-only recovery environment.
        subprocess.run(['mdel', '-i', str(image), '::BOOT/WALLPAPER.RAW'], check=True)
        for name in ('boot-app-snapshot', 'writable-desktop'):
            subprocess.run(['mdel', '-i', str(image), '::ETC/' + name], check=True)
        init = out / 'INIT.RC'
        init.write_bytes(INIT)
        subprocess.run(['mcopy', '-o', '-i', str(image), str(init), '::BOOT/INIT.RC'], check=True)
        if subprocess.check_output(['mtype', '-i', str(image), '::BOOT/INIT.RC']) != INIT:
            raise RuntimeError('recovery init script readback mismatch')
        identity = out / 'hardware-payload-id'
        identity.write_bytes(PAYLOAD_ID)
        subprocess.run(['mcopy', '-o', '-i', str(image), str(identity),
                        '::ETC/hardware-payload-id'], check=True)
        if fat('ETC/hardware-payload-id').stdout != PAYLOAD_ID:
            raise RuntimeError('recovery payload identity readback mismatch')
        identity.unlink()
        checker_info = embed_checker(image, checker)
        for name, expected in policy.items():
            read = fat('ETC/' + name)
            if read.returncode or read.stdout != expected:
                raise RuntimeError(f'maintenance policy changed: {name}')
        init.unlink()
        kernel = (out / 'KERNEL.ELF').read_bytes()
        initrd = image.read_bytes()
        selection = record(1, 0, kernel, initrd)
        (out / 'SELECT.BIN').write_bytes(selection)
        receipt = {'scope': 'prepared manual recovery only; not installed or boot tested',
                   'source_payload_id': manifest['payload_id'],
                   'source_manifest_sha256': sha(manifest_data),
                   'checker': checker_info,
                   'files': {name: {'bytes': len(data), 'sha256': sha(data)}
                             for name, data in (('KERNEL.ELF', kernel), ('INITRD.IMG', initrd),
                                                ('SELECT.BIN', selection))}}
        (out / 'manifest.json').write_text(json.dumps(receipt, indent=2) + '\n')
    except Exception:
        shutil.rmtree(out)
        raise
    return receipt


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', required=True, type=Path)
    p.add_argument('--checker', required=True, type=Path)
    p.add_argument('--elf-validator', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    a = p.parse_args()
    print(json.dumps(prepare(a.source.resolve(strict=True), a.checker,
                             a.elf_validator.resolve(strict=True), a.output.absolute()), indent=2))


if __name__ == '__main__':
    main()
