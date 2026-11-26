#!/usr/bin/env python3
"""Check secret mappings in a disposable QEMU guest. Full-core mode uses a localhost forward."""
import argparse
import json
import os
import shutil
import socket
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def request_full_dump(port, pid, deadline):
    while True:
        try:
            connection = socket.create_connection(('127.0.0.1', port), timeout=1)
            break
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.2)
    # The established guest request is sent once. A lost reply is a test
    # failure because the existing output path may already contain a core.
    with connection:
        connection.settimeout(20)
        connection.sendall(f'@1 proc dump {pid} /data/full.core --full\n'.encode())
        with connection.makefile('rb') as stream:
            records = []
            for _ in range(32):
                line = stream.readline(16384).strip()
                if line == b'BRIDGE_END 1':
                    break
                if line.startswith(b'{'):
                    records.append(json.loads(line))
            else:
                raise RuntimeError('No end marker in the full-core reply')
    if len(records) != 1:
        raise RuntimeError(f'Unexpected full-core reply {records}')
    reply = records[0]
    if (reply.get('dropped_data_pages') != 0 or reply.get('excluded_secret_ranges') != 2 or
            reply.get('excluded_secret_pages') != 2 or not reply.get('bytes')):
        raise RuntimeError(f'Unexpected full-core result {reply}')
    return reply


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/release')
    parser.add_argument('--timeout', type=float, default=90)
    parser.add_argument('--full', action='store_true', help='Also exercise the live full-dump command')
    args = parser.parse_args()
    if subprocess.run(['pgrep', '-x', 'vmware-vmx'], stdout=subprocess.DEVNULL).returncode == 0:
        parser.error('Workstation is running')
    output = args.output.resolve()
    output.mkdir(parents=True)
    guest = output / 'SECRET.ELF'
    subprocess.run([os.environ['MUSL_CC'], '-static', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                    ROOT / 'src/tests/test_secret_mappings_guest/main.c', '-o', guest], check=True)
    probe = output / 'probe'
    network_driver = ['--driver', 'virtio_net'] if args.full else []
    subprocess.run(['python3', ROOT / 'scripts/make_uefi_probe.py', '--build', args.build, '--output', probe,
                    '--display-width', '800', '--display-height', '600', *network_driver],
                   env=dict(os.environ, BROOK_INITRD_KIB='8192'), check=True)
    initrd = probe / 'payload/INITRD.IMG'
    marker = output / 'volatile-desktop'
    marker.write_text('1\n')
    subprocess.run(['mcopy', '-i', initrd, marker, '::ETC/volatile-desktop'], check=True)
    rc = output / 'INIT.RC'
    commands = 'set wm\nset vfb none\nkconsole\nrun --console --uid 0 /boot/SECRET.ELF'
    rc.write_text(commands + (' --full' if args.full else '') + '\n')
    subprocess.run(['mcopy', '-i', initrd, guest, '::BOOT/SECRET.ELF'], check=True)
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
    if args.full:
        with socket.socket() as reserved:
            reserved.bind(('127.0.0.1', 0))
            port = reserved.getsockname()[1]
        qemu += ['-device', 'virtio-net-pci,netdev=net0', '-netdev',
                 f'user,id=net0,restrict=on,hostfwd=tcp:127.0.0.1:{port}-:1234']
    else:
        qemu += ['-nic', 'none']
    with (output / 'qemu.log').open('w') as log:
        vm = subprocess.Popen(qemu, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + args.timeout
            full_reply = None
            while time.monotonic() < deadline:
                if serial.exists():
                    for line in serial.read_text(errors='replace').splitlines():
                        if args.full and full_reply is None and line.startswith('SECRET_CORE_FULL_READY '):
                            pid = int(line.split()[1])
                            full_reply = request_full_dump(port, pid, deadline)
                            (output / 'full-reply.json').write_text(json.dumps(full_reply, indent=2) + '\n')
                        if line.startswith('SECRET_CORE_PASS') and (not args.full or full_reply):
                            print(line)
                            return 0
                        if line.startswith('SECRET_CORE_FAIL') or 'KERNEL PANIC' in line:
                            raise RuntimeError(f'{line}. See {serial}')
                if vm.poll() is not None:
                    raise RuntimeError(f'QEMU exited early. See {output / "qemu.log"}')
                time.sleep(0.2)
            raise RuntimeError(f'Guest status timed out. See {serial}')
        finally:
            if vm.poll() is None:
                vm.terminate()
                try:
                    vm.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    vm.kill()
                    vm.wait()


if __name__ == '__main__':
    raise SystemExit(main())
