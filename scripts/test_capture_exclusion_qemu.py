#!/usr/bin/env python3
"""Check capture exclusion in an isolated headless QEMU guest."""
import argparse
import os
import shutil
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/release')
    parser.add_argument('--scale', type=int, choices=(100, 200), default=100)
    parser.add_argument('--timeout', type=float, default=90)
    args = parser.parse_args()
    if subprocess.run(['pgrep', '-x', 'vmware-vmx'], stdout=subprocess.DEVNULL).returncode == 0:
        parser.error('Workstation is running')
    output = args.output.resolve()
    output.mkdir(parents=True)
    guest = output / 'CAPTURE.ELF'
    subprocess.run([os.environ['MUSL_CC'], '-static', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                    ROOT / 'src/tests/test_capture_exclusion_guest/main.c', '-o', guest], check=True)
    probe = output / 'probe'
    subprocess.run(['python3', ROOT / 'scripts/make_uefi_probe.py', '--build', args.build, '--output', probe,
                    '--display-width', '800', '--display-height', '600'],
                   env=dict(os.environ, BROOK_INITRD_KIB='8192'), check=True)
    initrd = probe / 'payload/INITRD.IMG'
    marker = output / 'volatile-desktop'
    marker.write_text('1\n')
    subprocess.run(['mcopy', '-i', initrd, marker, '::ETC/volatile-desktop'], check=True)
    config = output / 'desktop-defaults.ini'
    config.write_text(f'[Desktop]\nscale={args.scale}\n')
    subprocess.run(['mcopy', '-o', '-i', initrd, config, '::ETC/desktop-defaults.ini'], check=True)
    rc = output / 'INIT.RC'
    commands = 'set wm\nset vfb none\nrun --console --uid 0 /boot/CAPTURE.ELF'
    rc.write_text(commands + '\n')
    subprocess.run(['mcopy', '-i', initrd, guest, '::BOOT/CAPTURE.ELF'], check=True)
    subprocess.run(['mcopy', '-o', '-i', initrd, rc, '::BOOT/INIT.RC'], check=True)
    disk = probe / 'probe.raw'
    subprocess.run(['mcopy', '-o', '-i', f'{disk}@@1048576', initrd, '::INITRD.IMG'], check=True)
    variables = output / 'VARS.fd'
    shutil.copyfile(os.environ['OVMF_VARS'], variables)
    serial = output / 'serial.log'
    kvm = os.access('/dev/kvm', os.R_OK | os.W_OK)
    qemu = ['qemu-system-x86_64', '-machine', 'q35',
            '-accel', 'kvm' if kvm else 'tcg', '-cpu', 'host' if kvm else 'max', '-smp', '4', '-m', '2048',
            '-nodefaults', '-drive', f'if=pflash,format=raw,readonly=on,file={os.environ["OVMF_CODE"]}',
            '-drive', f'if=pflash,format=raw,file={variables}', '-device', 'VGA', '-device', 'qemu-xhci,id=usb',
            '-drive', f'if=none,id=stick,format=raw,readonly=on,file={disk}',
            '-device', 'usb-storage,bus=usb.0,drive=stick,bootindex=1', '-display', 'none',
            '-serial', f'file:{serial}', '-no-reboot']
    qemu += ['-nic', 'none']
    with (output / 'qemu.log').open('w') as log:
        vm = subprocess.Popen(qemu, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                if serial.exists():
                    for line in serial.read_text(errors='replace').splitlines():
                        if line.startswith('CAPTURE_EXCLUSION_PASS'):
                            print(line)
                            return
                        if line.startswith('CAPTURE_EXCLUSION_FAIL'):
                            raise RuntimeError(line)
                if vm.poll() is not None:
                    raise RuntimeError(f'QEMU exited with status {vm.returncode}')
                time.sleep(0.2)
            raise RuntimeError(f'Timed out waiting for capture proof in {serial}')
        finally:
            vm.terminate()
            try:
                vm.wait(timeout=5)
            except subprocess.TimeoutExpired:
                vm.kill()
                vm.wait()


if __name__ == '__main__':
    main()
