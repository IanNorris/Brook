#!/usr/bin/env python3
"""Exercise normal and F5 boot on a disposable, pre-provisioned EFI image.

The image must have RECOVERY_MENU enabled and contain distinct normal and
recovery payloads. The recovery payload must emit the supplied marker on
serial; it must not emit the normal boot marker. The source is never written.
"""
import argparse
import shutil
import socket
import subprocess
import tempfile
import time
from pathlib import Path


def boot(image, firmware, work, recovery, marker, timeout):
    variables = work / 'vars.fd'
    serial = work / 'serial.log'
    monitor = work / 'monitor.sock'
    shutil.copyfile(firmware / 'OVMF_VARS.fd', variables)
    command = [
        'qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm', '-cpu', 'host',
        '-smp', '2', '-m', '1024', '-nodefaults',
        '-drive', f'if=pflash,format=raw,readonly=on,file={firmware / "OVMF_CODE.fd"}',
        '-drive', f'if=pflash,format=raw,file={variables}', '-device', 'VGA',
        '-device', 'qemu-xhci,id=usbc',
        '-drive', f'if=none,id=stick,format=raw,readonly=on,file={image}',
        '-device', 'usb-storage,bus=usbc.0,port=1,drive=stick,bootindex=1',
        '-display', 'none', '-serial', f'file:{serial}',
        '-monitor', f'unix:{monitor},server=on,wait=off', '-no-reboot',
    ]
    with (work / 'qemu.log').open('wb') as errors:
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=errors)
        try:
            deadline = time.monotonic() + timeout
            send_until = time.monotonic() + 12
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError('QEMU exited before the expected boot marker')
                if recovery and monitor.exists() and time.monotonic() < send_until:
                    try:
                        with socket.socket(socket.AF_UNIX) as connection:
                            connection.settimeout(1)
                            connection.connect(str(monitor))
                            connection.sendall(b'sendkey f5\n')
                    except OSError:
                        pass
                data = serial.read_bytes() if serial.exists() else b''
                if marker.encode() in data:
                    return
                time.sleep(0.2)
            raise RuntimeError(f'timed out waiting for {marker!r}; serial tail: {data[-1500:]!r}')
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--normal-marker', default='BOOT: complete')
    parser.add_argument('--recovery-marker', required=True)
    parser.add_argument('--timeout', type=int, default=60)
    args = parser.parse_args()
    image, firmware = args.image.resolve(strict=True), args.firmware.resolve(strict=True)
    if args.normal_marker == args.recovery_marker:
        parser.error('normal and recovery markers must be distinct')
    with tempfile.TemporaryDirectory(prefix='brook-recovery-menu-') as temp:
        for recovery, marker in ((False, args.normal_marker), (True, args.recovery_marker)):
            work = Path(temp) / ('recovery' if recovery else 'normal')
            work.mkdir()
            boot(image, firmware, work, recovery, marker, args.timeout)
            print(('RECOVERY' if recovery else 'NORMAL') + '_BOOT_PASS', flush=True)


if __name__ == '__main__':
    main()
