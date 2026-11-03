#!/usr/bin/env python3
"""Check a prepared recovery test image in QEMU; never write the source.

The image must contain an F5 recovery pair with the synthetic QEMU identity
used below, a disposable ext2 second partition and a bundled static checker.
Only a virtual USB network adapter is present, not USB storage.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/vm_bridge'))
from hardware import Hardware
from prepare_ssd_recovery_pair import PAYLOAD_ID

START = 133120 * 512
LENGTH = 126976 * 512


def outside_partition(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        remaining = START
        while remaining:
            block = stream.read(min(1024 * 1024, remaining))
            if not block:
                raise RuntimeError('Short test image')
            digest.update(block)
            remaining -= len(block)
        stream.seek(START + LENGTH)
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.digest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--firmware', required=True, type=Path)
    parser.add_argument('--repair', action='store_true',
                        help='Corrupt and repair a temporary image copy, then verify it on the host')
    args = parser.parse_args()
    image = args.image.resolve(strict=True)
    firmware = args.firmware.resolve(strict=True)
    if not image.is_file() or image.stat().st_size != 262144 * 512:
        parser.error('expected a 128 MiB disposable regular-file test image')
    table = json.loads(subprocess.check_output(['sfdisk', '--json', str(image)]))['partitiontable']
    partitions = table.get('partitions', [])
    if (table.get('label') != 'gpt' or table.get('sectorsize') != 512 or len(partitions) != 2 or
            partitions[1].get('start') != 133120 or partitions[1].get('size') != 126976 or
            partitions[1].get('type', '').lower() != '0fc63daf-8483-4772-8e79-3d69d8477de4'):
        parser.error('test image partition 2 does not match the disposable fixture geometry')
    with tempfile.TemporaryDirectory(prefix='brook-recovery-fsck-') as directory:
        work = Path(directory)
        if args.repair:
            # Only this newly created regular-file copy is ever made writable.
            scratch = work / 'disk.raw'
            shutil.copyfile(image, scratch)
            original_outside = outside_partition(scratch)
            partition = work / 'data.img'
            subprocess.run(['dd', f'if={scratch}', f'of={partition}', 'bs=512',
                            'skip=133120', 'count=126976', 'status=none'], check=True)
            subprocess.run(['debugfs', '-w', '-R', 'setb 10000', str(partition)],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(['debugfs', '-w', '-R', 'set_super_value state 0', str(partition)],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            check = subprocess.run(['e2fsck', '-f', '-n', str(partition)],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if check.returncode != 4 or b'bitmap differences' not in check.stdout.lower():
                raise RuntimeError('Disposable fixture did not develop the expected bitmap error')
            subprocess.run(['dd', f'if={partition}', f'of={scratch}', 'bs=512',
                            'seek=133120', 'conv=notrunc', 'status=none'], check=True)
            partition.unlink()
            image = scratch
        shutil.copyfile(firmware / 'OVMF_VARS.fd', work / 'vars.fd')
        with socket.socket() as reserved:
            reserved.bind(('127.0.0.1', 0))
            port = reserved.getsockname()[1]
        monitor = work / 'monitor.sock'
        command = [
            'qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm', '-cpu', 'host',
            '-smp', '4', '-m', '2048', '-nodefaults',
            '-drive', f'if=pflash,format=raw,readonly=on,file={firmware / "OVMF_CODE.fd"}',
            '-drive', f'if=pflash,format=raw,file={work / "vars.fd"}', '-device', 'VGA',
            '-drive', f'if=none,id=sata,format=raw,{"snapshot=off" if args.repair else "snapshot=on"},file={image}',
            '-device', 'ide-hd,drive=sata,bus=ide.0,bootindex=1,serial=BROOK-RECOVERY-QA',
            '-device', 'qemu-xhci,id=usbc', '-device', 'usb-net,bus=usbc.0,port=2,netdev=net',
            '-netdev', f'user,id=net,restrict=on,hostfwd=tcp:127.0.0.1:{port}-:1234',
            '-display', 'none', '-serial', f'file:{work / "serial.log"}',
            '-monitor', f'unix:{monitor},server=on,wait=off', '-no-reboot',
        ]
        with (work / 'qemu.log').open('wb') as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            start = time.monotonic()

            stop_keys = threading.Event()

            def select_recovery():
                while not stop_keys.is_set() and process.poll() is None and time.monotonic() - start < 12:
                    if monitor.exists():
                        try:
                            with socket.socket(socket.AF_UNIX) as connection:
                                connection.settimeout(1)
                                connection.connect(str(monitor))
                                connection.sendall(b'sendkey f5\n')
                        except OSError:
                            pass
                    stop_keys.wait(0.2)

            key_thread = threading.Thread(target=select_recovery, daemon=True)
            key_thread.start()
            try:
                bridge = Hardware(work / 'registry')
                while True:
                    if process.poll() is not None:
                        raise RuntimeError('QEMU exited: ' + (work / 'qemu.log').read_text(errors='replace')[-2000:])
                    try:
                        attached = bridge.attach('127.0.0.1', port)
                        break
                    except (OSError, RuntimeError, ValueError):
                        if time.monotonic() - start > 90:
                            raise TimeoutError('Recovery debug bridge did not become available')
                        time.sleep(0.4)
                session = attached['session']
                stop_keys.set()
                key_thread.join(timeout=3)
                if key_thread.is_alive():
                    raise RuntimeError('F5 injection did not stop')
                until = time.monotonic() + 15
                serial = work / 'serial.log'
                while not serial.exists() or PAYLOAD_ID.strip() not in serial.read_bytes():
                    if time.monotonic() > until:
                        raise RuntimeError('Distinct recovery payload marker not observed')
                    time.sleep(0.2)
                disk = bridge.command(session, 'bridge hardware ahci-ro')
                if (not disk.get('ready') or disk.get('reason') != 'configured ATA device writable' or
                        disk.get('sectors') != 262144 or disk.get('model') != 'QEMU HARDDISK' or
                        disk.get('serial') != 'BROOK-RECOVERY-QA'):
                    raise RuntimeError(f'Unexpected test disk identity: {disk}')
                if attached['guest']['writable_desktop']['ready']:
                    raise RuntimeError('Recovery must not mount the desktop filesystem')
                def checker(options, accepted):
                    request = uuid.uuid4().hex
                    bridge.guest_run(session, request,
                                     ['/boot/E2FSCK.ELF', *options, '/dev/brook-install-disk2'],
                                     cwd='/tmp', uid=0, wait_s=2)
                    deadline = time.monotonic() + 60
                    while True:
                        result = bridge.job(session, request, wait_s=2)
                        if result['state'] in ('completed', 'failed', 'lost', 'rejected'):
                            break
                        if time.monotonic() > deadline:
                            raise TimeoutError('Inspect checker job ' + request)
                    if result['state'] != 'completed' or result['exit']['code'] not in accepted:
                        raise RuntimeError(f'Recovery checker failed: {result}')
                    return result

                if args.repair:
                    checker(['-p'], (1,))
                result = checker(['-f', '-n'], (0,))
                if 'Pass 5' not in result['stdout']['text']:
                    raise RuntimeError(f'Full verification did not finish: {result}')
                if args.repair:
                    request = uuid.uuid4().hex
                    state = bridge.power(session, request, 'shutdown')
                    deadline = time.monotonic() + 60
                    while state['state'] != 'ready':
                        if state['state'] in ('failed', 'unsupported') or time.monotonic() > deadline:
                            raise RuntimeError(f'Checked shutdown failed: {state}')
                        time.sleep(0.2)
                        state = bridge.power(session, request, 'status')
                print('RECOVERY_GUEST_FSCK_PASS' if args.repair else 'RECOVERY_SATA_NO_USB_FSCK_PASS', flush=True)
            finally:
                stop_keys.set()
                key_thread.join(timeout=3)
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        if args.repair:
            subprocess.run(['dd', f'if={image}', f'of={partition}', 'bs=512',
                            'skip=133120', 'count=126976', 'status=none'], check=True)
            result = subprocess.run(['e2fsck', '-f', '-n', str(partition)],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if result.returncode:
                raise RuntimeError(f'Host verification failed: {result.stdout[-2000:]!r}')
            if outside_partition(image) != original_outside:
                raise RuntimeError('Recovery unexpectedly changed bytes outside partition 2')
            print('RECOVERY_SATA_REPAIR_HOST_VERIFY_PASS', flush=True)


if __name__ == '__main__':
    main()
