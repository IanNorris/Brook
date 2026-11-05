#!/usr/bin/env python3
"""Exercise the explicitly armed panic reset on disposable QEMU disk copies."""
import argparse
from contextlib import redirect_stdout
from io import StringIO
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT / 'tools'))
import crash_decoder


def check_reason(serial, expected):
    packet=crash_decoder.parse_serial_log(str(serial))
    output=StringIO()
    with redirect_stdout(output):
        crash_decoder.decode_crash(packet,None,False,True)
    decoded=json.loads(output.getvalue())
    actual=decoded.get('message')
    if actual!=expected:raise RuntimeError(f'panic reason differs: {actual!r}')
    log=decoded.get('log_tail')
    if not log or not log['available'] or 'BOOT' not in log['text']:
        raise RuntimeError('panic packet lacks available recent boot log')
    return decoded


def run_case(root, image, kernel, firmware, command, armed, timeout, verify_reboot=False, expect_denied=False, headless=False):
    disk = root / 'disk.raw'
    shutil.copyfile(image, disk)
    esp = str(disk) + '@@1048576'
    initrd = root / 'initrd.img'
    script = root / 'INIT.RC'
    subprocess.run(['mcopy', '-i', esp, '::/INITRD.IMG', str(initrd)], check=True)
    script.write_text(command + '\n')
    subprocess.run(['mcopy', '-o', '-i', str(initrd), str(script), '::/BOOT/INIT.RC'], check=True)
    subprocess.run(['mcopy', '-o', '-i', esp, str(initrd), '::/INITRD.IMG'], check=True)
    subprocess.run(['mcopy', '-o', '-i', esp, str(kernel), '::/KERNEL/BROOK.ELF'], check=True)
    vars_file = root / 'vars.fd'
    shutil.copyfile(firmware / 'OVMF_VARS.fd', vars_file)
    serial = root / 'serial.log'
    args = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'tcg', '-cpu', 'max',
            '-smp', '2', '-m', '1024', '-nodefaults',
            '-drive', f'if=pflash,format=raw,readonly=on,file={firmware / "OVMF_CODE.fd"}',
            '-drive', f'if=pflash,format=raw,file={vars_file}', '-device', 'VGA',
            '-drive', f'if=none,id=stick,format=raw,file={disk}',
            '-device', 'ide-hd,drive=stick,bus=ide.0,bootindex=1', '-display', 'none',
            '-serial', f'file:{serial}',
            '-fw_cfg', 'name=opt/brook/power,string=q35-reset']
    if not verify_reboot:
        args += ['-no-reboot']
    if armed:
        args += ['-fw_cfg', 'name=opt/brook/panic-test-reset,string=test-1s']
    if headless:
        args += ['-fw_cfg', 'name=opt/brook/panic-test-headless,string=yes']
    with (root / 'qemu.log').open('wb') as errors:
        process = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=errors)
        try:
            deadline = time.monotonic() + timeout
            seen_dump = None
            while time.monotonic() < deadline:
                output = serial.read_bytes() if serial.exists() else b''
                if expect_denied and b'crashtest: seriallock requires QEMU reset test' in output:
                    if b'PANIC_HEX:' in output or b'PANIC_TEST_RESET_WAIT' in output:
                        raise RuntimeError('unarmed serial-lock test unexpectedly panicked')
                    return
                marker = b'PANIC_TEST_RESET_WAIT' if armed else b'PANIC QR: rendered to framebuffer'
                if seen_dump is None and marker in output:
                    seen_dump = time.monotonic()
                status = process.poll()
                if verify_reboot and output.count(b'PANIC_TEST_RESET_ARMED') >= 2:
                    if seen_dump is None or time.monotonic() - seen_dump < 0.7:
                        raise RuntimeError('reboot occurred before the panic delay')
                    if status is not None or b'PANIC_HEX:' not in output:
                        raise RuntimeError('reset did not produce a running second boot with panic evidence')
                    return
                if status is not None:
                    if not armed or status != 0 or seen_dump is None:
                        raise RuntimeError(f'unexpected QEMU exit {status}: {output[-3000:]!r}')
                    if b'PANIC_TEST_RESET_ARMED' not in output or b'PANIC_TEST_RESET_WAIT' not in output:
                        raise RuntimeError('QEMU exited without the armed reset markers')
                    if command=='crashtest ud' and b'=== KERNEL FAULT ===' not in output:
                        raise RuntimeError('exception evidence was not emitted')
                    if b'PANIC_HEX:' not in output:
                        raise RuntimeError('structured panic evidence was not emitted')
                    if b'PANIC_TEST_RESET_TIMER_FAILED' in output or b'PANIC_TEST_RESET_RETURNED' in output:
                        raise RuntimeError('panic reset failed or returned')
                    if time.monotonic() - seen_dump < 0.7:
                        raise RuntimeError('reset occurred before the required panic delay')
                    return
                if not armed and seen_dump is not None and time.monotonic() - seen_dump > 2.5:
                    if b'PANIC_TEST_RESET_ARMED' in output or b'PANIC_TEST_RESET_WAIT' in output:
                        raise RuntimeError('unarmed boot entered the test reset path')
                    return
                time.sleep(0.05)
            raise RuntimeError(f'timed out: {output[-3000:]!r}')
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True,
                        help='Disposable-compatible GPT fixture with ESP at sector 2048')
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=90)
    args = parser.parse_args()
    image = args.image.resolve(strict=True)
    kernel = args.kernel.resolve(strict=True)
    firmware = args.firmware.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='brook-panic-reset-') as temp:
        for name, command, armed in [('panic', 'panic panic_reset_test', True),
                                     ('unarmed', 'panic unarmed_control', False),
                                     ('exception', 'crashtest ud', True)]:
            root = Path(temp) / name
            root.mkdir()
            run_case(root, image, kernel, firmware, command, armed, args.timeout)
            check_reason(root/'serial.log', 'Unrecoverable kernel exception' if name=='exception' else
                         f'SHELL: {command.split(" ",1)[1]}\n')
            print(f'{name}: PASS', flush=True)
        root = Path(temp) / 'reboot'
        root.mkdir()
        run_case(root, image, kernel, firmware, 'panic reboot_test', True, args.timeout, True)
        check_reason(root/'serial.log', 'SHELL: reboot_test\n')
        print('reboot: PASS', flush=True)
        root = Path(temp) / 'seriallock'
        root.mkdir()
        run_case(root, image, kernel, firmware, 'crashtest seriallock', True, args.timeout, True)
        check_reason(root/'serial.log', 'crashtest: panic while serial lock held')
        print('panic while holding serial lock and reboot: PASS', flush=True)
        root = Path(temp) / 'seriallock-unarmed'
        root.mkdir()
        run_case(root, image, kernel, firmware, 'crashtest seriallock', False, args.timeout, expect_denied=True)
        print('unarmed serial-lock test rejected: PASS', flush=True)
        root = Path(temp) / 'headless-exception'
        root.mkdir()
        run_case(root, image, kernel, firmware, 'crashtest ud', True, args.timeout,
                 verify_reboot=True, headless=True)
        decoded=check_reason(root/'serial.log', 'Unrecoverable kernel exception')
        if decoded.get('exception',{}).get('vector')!=6 or 'RIP' not in decoded.get('registers',{}) or not decoded.get('system_info',{}).get('git_hash'):
            raise RuntimeError('headless exception packet missing vector, RIP or build identity')
        print('headless exception packet and reboot: PASS', flush=True)


if __name__ == '__main__':
    main()
