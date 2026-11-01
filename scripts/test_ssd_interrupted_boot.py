#!/usr/bin/env python3
"""Check stopped private QEMU SSDs after injected migration interruptions."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from test_hardware_power import Qmp


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    a = parser.parse_args()
    if a.fixture.is_symlink() or not a.fixture.is_file() or a.fixture.stat().st_size != 500118192 * 512:
        raise ValueError('expected a stopped, Dell-sized regular-file QEMU fixture')
    out = a.output.absolute(); out.mkdir(parents=True, exist_ok=False)
    partition = out / 'partition.img'
    subprocess.run(['dd', 'if=' + str(a.fixture), 'of=' + str(partition), 'bs=4M', 'skip=1048576',
                    'count=6442450944', 'iflag=skip_bytes,count_bytes', 'conv=sparse', 'status=none'], check=True)
    with (out / 'fsck-fat.log').open('w') as log:
        subprocess.run(['fsck.fat', '-n', str(partition)], stdout=log, stderr=subprocess.STDOUT, check=True)
    partition.unlink()  # disposable extraction; retain the check log
    variables = out / 'VARS.fd'; shutil.copy2(os.environ['OVMF_VARS'], variables); variables.chmod(0o600)
    args = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm', '-cpu', 'host', '-smp', '4', '-m', '4096', '-nodefaults',
            '-drive', 'if=pflash,format=raw,readonly=on,file=' + os.environ['OVMF_CODE'],
            '-drive', 'if=pflash,format=raw,file=' + str(variables), '-device', 'VGA,vgamem_mb=64',
            '-drive', f'if=none,id=sata,format=raw,snapshot=on,file={a.fixture.absolute()}',
            '-device', 'ide-hd,drive=sata,bus=ide.0,bootindex=1,serial=BROOK-SYSTEM-TEST01',
            '-device', 'qemu-xhci,id=usbc', '-display', 'none', '-serial', 'file:' + str(out / 'serial.log'),
            '-qmp', 'unix:' + str(out / 'qmp.sock') + ',server=on,wait=off', '-no-reboot']
    with (out / 'qemu.log').open('x') as log:
        owner = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        qmp = None
        try:
            end = time.monotonic() + 180
            while time.monotonic() < end:
                if owner.poll() is not None: raise RuntimeError('guest exited before desktop boot: ' + str(owner.returncode))
                serial = (out / 'serial.log').read_text(errors='replace') if (out / 'serial.log').exists() else ''
                if 'PANIC_HEX:' in serial: raise RuntimeError('guest panic')
                if 'BOOT: complete' in serial: break
                time.sleep(.2)
            else: raise TimeoutError('guest did not finish boot')
            qmp = Qmp(out / 'qmp.sock'); qmp.call('quit'); assert owner.wait(timeout=15) == 0
        finally:
            if qmp: qmp.close()
            if owner.poll() is None: owner.terminate(); owner.wait(timeout=15)
    (out / 'result.json').write_text(json.dumps({'passed': True, 'fat_check': True, 'guest_boot': True,
        'scope': 'private QEMU process interruption; not physical power-loss proof'}, indent=2) + '\n')
    print('SSD_INTERRUPTED_BOOT_PASS', a.fixture.parent.name, flush=True)


if __name__ == '__main__': main()
