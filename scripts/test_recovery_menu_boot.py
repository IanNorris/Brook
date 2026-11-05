#!/usr/bin/env python3
"""Exercise normal and F5 boot on a disposable, pre-provisioned EFI image.

The image must have RECOVERY_MENU enabled and contain distinct normal and
recovery payloads. The recovery payload must emit the supplied recovery
marker on serial; the normal payload must emit the normal marker. Use a
normal marker that uniquely identifies the normal payload when available.
The source image is never written.
"""
import argparse
import shutil
import socket
import subprocess
import tempfile
import time
from pathlib import Path


def boot(image, firmware, work, recovery, marker, timeout, sata, forbidden=None, sata_snapshot=True,
         hold_after_marker=0, sata_serial=None, failure_marker=None, extra_fw_cfg=None, expect_reset=False):
    variables = work / 'vars.fd'
    serial = work / 'serial.log'
    monitor = work / 'monitor.sock'
    shutil.copyfile(firmware / 'OVMF_VARS.fd', variables)
    command = [
        'qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm', '-cpu', 'host',
        '-smp', '2', '-m', '1024', '-nodefaults',
        '-drive', f'if=pflash,format=raw,readonly=on,file={firmware / "OVMF_CODE.fd"}',
        '-drive', f'if=pflash,format=raw,file={variables}', '-device', 'VGA',
    ]
    if sata:
        command += [
            '-drive', f'if=none,id=stick,format=raw,snapshot={"on" if sata_snapshot else "off"},file={image}',
            '-device', 'ide-hd,drive=stick,bus=ide.0,bootindex=1' +
                       (f',serial={sata_serial}' if sata_serial else ''),
        ]
    else:
        command += [
        '-device', 'qemu-xhci,id=usbc',
        '-drive', f'if=none,id=stick,format=raw,readonly=on,file={image}',
        '-device', 'usb-storage,bus=usbc.0,port=1,drive=stick,bootindex=1',
        ]
    command += [
        '-display', 'none', '-serial', f'file:{serial}',
        '-monitor', f'unix:{monitor},server=on,wait=off', '-no-reboot',
    ]
    for item in extra_fw_cfg or ():
        command += ['-fw_cfg',item]
    with (work / 'qemu.log').open('wb') as errors:
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=errors)
        try:
            deadline = time.monotonic() + timeout
            send_until = time.monotonic() + 12
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError('QEMU exited before the expected boot marker: ' +
                                       (work / 'qemu.log').read_text(errors='replace')[-2000:] +
                                       '\nserial tail: ' + (serial.read_text(errors='replace')[-3000:] if serial.exists() else ''))
                if recovery and monitor.exists() and time.monotonic() < send_until:
                    try:
                        with socket.socket(socket.AF_UNIX) as connection:
                            connection.settimeout(1)
                            connection.connect(str(monitor))
                            connection.sendall(b'sendkey f5\n')
                    except OSError:
                        pass
                data = serial.read_bytes() if serial.exists() else b''
                if failure_marker and failure_marker.encode() in data:
                    raise RuntimeError(f'boot reported {failure_marker!r}; serial tail: {data[-10000:]!r}')
                if forbidden and forbidden.encode() in data:
                    raise RuntimeError(f'unexpected boot marker {forbidden!r}')
                if marker.encode() in data:
                    if expect_reset:
                        try: status=process.wait(timeout=15)
                        except subprocess.TimeoutExpired: raise RuntimeError('QEMU did not reset after panic marker')
                        if status!=0 or b'PANIC_TEST_RESET_WAIT' not in serial.read_bytes():
                            raise RuntimeError('panic reset did not complete successfully')
                        return
                    until = time.monotonic() + hold_after_marker
                    while time.monotonic() < until:
                        if process.poll() is not None:
                            raise RuntimeError('QEMU exited after the expected boot marker')
                        if forbidden and forbidden.encode() in serial.read_bytes():
                            raise RuntimeError(f'unexpected boot marker {forbidden!r}')
                        time.sleep(0.1)
                    return
                time.sleep(0.2)
            raise RuntimeError(f'timed out waiting for {marker!r}; serial tail: {data[-10000:]!r}')
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
    parser.add_argument('--sata', action='store_true',
                        help='Boot via SATA with no USB storage device')
    parser.add_argument('--timeout', type=int, default=60)
    args = parser.parse_args()
    image, firmware = args.image.resolve(strict=True), args.firmware.resolve(strict=True)
    if args.normal_marker == args.recovery_marker:
        parser.error('normal and recovery markers must be distinct')
    with tempfile.TemporaryDirectory(prefix='brook-recovery-menu-') as temp:
        for recovery, marker in ((False, args.normal_marker), (True, args.recovery_marker)):
            work = Path(temp) / ('recovery' if recovery else 'normal')
            work.mkdir()
            boot(image, firmware, work, recovery, marker, args.timeout, args.sata)
            print(('RECOVERY' if recovery else 'NORMAL') + '_BOOT_PASS', flush=True)


if __name__ == '__main__':
    main()
