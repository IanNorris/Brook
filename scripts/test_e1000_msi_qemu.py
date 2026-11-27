#!/usr/bin/env python3
"""Compare polling and MSI on QEMU's 82574L using the same kernel and payload.

Run from nix-shell with matching kernel/modules and a plain UEFI loader
arranged as for make_uefi_probe.py. Use an ephemeral bridge operator key.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import statistics
import subprocess
import sys
import time

from build_rtx_smoke_files import fat_write

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/vm_bridge'))
from hardware import Hardware

BLOCK = bytes((i * 17 + 3) % 251 for i in range(65536))
BLOCKS = 128


def unused_port():
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        return listener.getsockname()[1]


def transfer(port, download):
    with socket.create_connection(('127.0.0.1', port), timeout=90) as connection:
        with connection.makefile('rb') as stream:
            start = time.monotonic()
            connection.sendall(b'D' if download else b'U')
            for _ in range(BLOCKS):
                if download:
                    if stream.read(len(BLOCK)) != BLOCK:
                        raise RuntimeError('Incomplete or corrupted TCP download')
                else:
                    connection.sendall(BLOCK)
            if not download and stream.read(1) != b'+':
                raise RuntimeError('Guest did not verify upload')
            return (len(BLOCK) * BLOCKS * 8 / 1e6) / (time.monotonic() - start)


def run(mode, base, original, benchmark, args):
    base.mkdir()
    image = base / 'probe.raw'
    initrd = base / 'INITRD.IMG'
    shutil.copyfile(original / 'probe.raw', image)
    shutil.copyfile(original / 'payload/INITRD.IMG', initrd)
    fat_write(initrd, 'BOOT/BENCH.ELF', benchmark.read_bytes())
    fat_write(initrd, 'BOOT/INIT.RC', b'set wm\nset vfb none\nkconsole\nhwprobe on\nrun /boot/BENCH.ELF\n')
    if mode == 'msi':
        fat_write(initrd, 'ETC/e1000-msi', b'1\n')
    subprocess.run(['mcopy', '-o', '-i', f'{image}@@1048576', initrd, '::INITRD.IMG'], check=True)

    variables = base / 'VARS.fd'
    shutil.copyfile(os.environ['OVMF_VARS'], variables)
    debug = unused_port()
    bulk = unused_port()
    while bulk == debug:
        bulk = unused_port()
    serial = base / 'serial.log'
    argv = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm',
            '-cpu', 'host', '-smp', '4', '-m', '2048', '-nodefaults',
            '-display', 'none', '-serial', f'file:{serial}', '-no-reboot',
            '-drive', f'if=pflash,format=raw,readonly=on,file={os.environ["OVMF_CODE"]}',
            '-drive', f'if=pflash,format=raw,file={variables}', '-device', 'VGA',
            '-drive', f'if=none,id=boot,format=raw,readonly=on,file={image}',
            '-device', 'qemu-xhci,id=usb', '-device', 'usb-storage,bus=usb.0,drive=boot,bootindex=1',
            '-device', 'pcie-root-port,chassis=1,slot=1,id=nicbus',
            '-device', 'e1000e,bus=nicbus,netdev=lan', '-netdev',
            f'user,id=lan,restrict=on,hostfwd=tcp:127.0.0.1:{debug}-:1234,hostfwd=tcp:127.0.0.1:{bulk}-:4211']
    (base / 'argv.json').write_text(json.dumps(argv, indent=2) + '\n')
    with (base / 'qemu.log').open('w') as errors:
        guest = subprocess.Popen(argv, stdout=errors, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 100
            while time.monotonic() < deadline and guest.poll() is None:
                boot_log = serial.read_text(errors='replace') if serial.exists() else ''
                if 'net: IP 10.0.2.15 (DHCP)' in boot_log and '/boot/BENCH.ELF' in boot_log:
                    break
                time.sleep(.1)
            else:
                raise RuntimeError(f'{mode} did not reach the benchmark: {serial}')

            bridge = Hardware(base / 'registry')
            session = bridge.attach('127.0.0.1', debug, operator_key_file=str(args.operator_seed))['session']

            def snapshot():
                return bridge.command(session, 'bridge network')['network_driver_snapshot']

            time.sleep(.5)
            before = snapshot()
            time.sleep(3)
            after_idle = snapshot()
            download = []
            upload = []
            for _ in range(args.repetitions):
                download.append(transfer(bulk, True))
                upload.append(transfer(bulk, False))
            final = snapshot()
            return {'download_mbps': download, 'upload_mbps': upload,
                    'idle_net_poll_cycles': after_idle['net_poll_cycles'] - before['net_poll_cycles'],
                    'transfer_msi_wakes': final['msi_wakes'] - after_idle['msi_wakes'],
                    'before': before, 'after_idle': after_idle, 'final': final}
        finally:
            if guest.poll() is None:
                guest.terminate()
                try:
                    guest.wait(5)
                except subprocess.TimeoutExpired:
                    guest.kill()
                    guest.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--operator-seed', type=lambda arg: Path(arg).resolve(), required=True)
    parser.add_argument('--output', type=lambda arg: Path(arg).resolve(), required=True)
    parser.add_argument('--repetitions', type=int, default=3, choices=range(1, 11))
    args = parser.parse_args()
    if subprocess.run(['pgrep', '-x', 'vmware-vmx'], stdout=subprocess.DEVNULL).returncode == 0:
        raise SystemExit('VMware is running')
    args.output.mkdir(parents=True)
    original = args.output / 'fixture'
    guest = args.output / 'BENCH.ELF'
    subprocess.run([os.environ['MUSL_CC'], '-static', '-O2', '-Wall', '-Wextra', '-Werror',
                    ROOT / 'src/tests/test_e1000/guest.c', '-o', guest], check=True)
    subprocess.run([sys.executable, ROOT / 'scripts/make_uefi_probe.py', '--output', original,
                    '--build', args.build, '--driver', 'e1000', '--usb-read-only',
                    '--display-width', '800', '--display-height', '600'], check=True)
    results = {}
    for mode in ('poll', 'msi'):
        results[mode] = run(mode, args.output / mode, original, guest, args)
        (args.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    poll, msi = results['poll'], results['msi']
    assert poll['final']['msi_enabled'] == 0 and poll['transfer_msi_wakes'] == 0
    assert msi['final']['msi_enabled'] == 1 and msi['transfer_msi_wakes'] > 0
    assert msi['idle_net_poll_cycles'] < poll['idle_net_poll_cycles'] / 2
    # Treat throughput differences within 10% as equivalent under QEMU.
    for key in ('download_mbps', 'upload_mbps'):
        assert statistics.median(msi[key]) >= .9 * statistics.median(poll[key]), results
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
