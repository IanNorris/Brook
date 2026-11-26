#!/usr/bin/env python3
"""Exercise console close with each production shortcut in a headless WM fixture."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

from prepare_hardware_kernel_pair import KERNEL_CONSOLE_SHORTCUT_PATH, hide_kernel_console
from test_input_preferences_qemu import send_key
from test_rtx_smoke_qemu import Qmp, qmp_button

ROOT = Path(__file__).resolve().parents[1]
CREATED = re.compile(r"WM: created window (\d+) 'Kernel Console' at \((\d+),(\d+)\) (\d+)x\d+.* for pid (\d+)")
DESTROYED = re.compile(r"WM: destroyed window (\d+) 'Kernel Console'")


def read_fat(image, path):
    return subprocess.check_output(['mtype', '-i', image, '::/' + path])


def cursor_to(qmp, x, y):
    def move(dx, dy):
        qmp.call('input-send-event', events=[
            {'type': 'rel', 'data': {'axis': 'x', 'value': dx}},
            {'type': 'rel', 'data': {'axis': 'y', 'value': dy}},
        ])
        time.sleep(.04)

    # Clamp the relative USB mouse to the origin, then aim in screen pixels.
    for _ in range(10):
        move(-110, -110)
    while x or y:
        dx, dy = min(x, 110), min(y, 110)
        move(dx, dy)
        x -= dx
        y -= dy


def exercise(folder, shortcut_image, build, dell, accel):
    folder.mkdir()
    probe = folder / 'probe'
    subprocess.run([sys.executable, ROOT / 'scripts/make_uefi_probe.py',
                    '--build', build, '--output', probe, '--display-width', '1024',
                    '--display-height', '768', '--driver', 'xhci'], check=True)
    initrd = probe / 'payload/INITRD.IMG'
    startup = folder / 'INIT.RC'
    startup.write_bytes(hide_kernel_console(read_fat(initrd, 'BOOT/INIT.RC')) if dell
                        else b'set wm\nset vfb none\n')
    assert b'kconsole' not in read_fat(shortcut_image, 'BOOT/INIT.RC').splitlines()
    shortcut = folder / 'KCONSOLE.RC'
    shortcut.write_bytes(read_fat(shortcut_image, KERNEL_CONSOLE_SHORTCUT_PATH))
    assert shortcut.read_bytes() == (ROOT / 'data/shortcuts/kernel_console.rc').read_bytes()
    subprocess.run(['mmd', '-i', initrd, '::/BOOT/SHORTCUTS'], check=True)
    for source, dest in ((startup, 'BOOT/INIT.RC'), (shortcut, KERNEL_CONSOLE_SHORTCUT_PATH)):
        subprocess.run(['mcopy', '-o', '-i', initrd, source, '::/' + dest], check=True)
    disk = probe / 'probe.raw'
    subprocess.run(['mcopy', '-o', '-i', f'{disk}@@1048576', initrd, '::INITRD.IMG'], check=True)
    variables = folder / 'VARS.fd'
    shutil.copyfile(os.environ['OVMF_VARS'], variables)
    serial, sock = folder / 'serial.log', folder / 'qmp.sock'
    command = ['qemu-system-x86_64', '-machine', 'q35,i8042=off', '-accel', accel,
               '-cpu', 'host' if accel == 'kvm' else 'max', '-smp', '4', '-m', '1024',
               '-nodefaults', '-display', 'none', '-nic', 'none', '-no-reboot',
               '-drive', f'if=pflash,format=raw,readonly=on,file={os.environ["OVMF_CODE"]}',
               '-drive', f'if=pflash,format=raw,file={variables}', '-device', 'VGA',
               '-device', 'qemu-xhci,id=usb',
               '-drive', f'if=none,id=stick,format=raw,readonly=on,file={disk}',
               '-device', 'usb-storage,bus=usb.0,port=1,drive=stick,bootindex=1',
               '-device', 'usb-kbd,bus=usb.0,port=2', '-device', 'usb-mouse,bus=usb.0,port=3',
               '-serial', f'file:{serial}', '-qmp', f'unix:{sock},server=on,wait=off']
    with (folder / 'qemu.log').open('w') as log:
        vm = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        qmp = None
        try:
            def wait(predicate, description, seconds=30):
                deadline = time.monotonic() + seconds
                while vm.poll() is None and time.monotonic() < deadline:
                    text = serial.read_text(errors='replace') if serial.exists() else ''
                    if predicate(text):
                        return text
                    time.sleep(.05)
                raise RuntimeError(f'{description} did not occur, see {serial}')

            log = wait(lambda s: 'BOOT: desktop presented' in s, 'desktop', 120)
            assert not CREATED.search(log)
            qmp = Qmp(sock)
            pids = []

            def open_console():
                send_key(qmp, 'meta_l')
                for key in ('k', 'e', 'r', 'n', 'e', 'l', 'spc', 'c', 'o', 'n', 's', 'o', 'l', 'e'):
                    send_key(qmp, key)
                send_key(qmp, 'ret')
                text = wait(lambda s: len(CREATED.findall(s)) > len(pids), 'Apps > Kernel Console')
                idx, x, y, width, pid = map(int, CREATED.findall(text)[-1])
                assert pid not in pids
                pids.append(pid)
                time.sleep(.25)
                return idx, x, y, width

            idx, x, y, width = open_console()
            cursor_to(qmp, x+width-10, y+14)
            qmp_button(qmp, 'left')
            wait(lambda s: DESTROYED.findall(s) == [str(idx)], 'caption close')
            idx2, x, y, _ = open_console()
            cursor_to(qmp, x+120, y+60)
            qmp_button(qmp, 'left')
            send_key(qmp, 'alt', 'f4')
            wait(lambda s: DESTROYED.findall(s) == [str(idx), str(idx2)], 'Alt+F4 close')
            open_console()
            qmp.call('screendump', filename=str(folder / 'reopened.ppm'))
            return {'close_button': True, 'alt_f4': True, 'console_pids': pids}
        finally:
            if qmp is not None:
                qmp.f.close()
                qmp.s.close()
            vm.terminate()
            try:
                vm.wait(timeout=5)
            except subprocess.TimeoutExpired:
                vm.kill()
                vm.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('dell-initrd', 'rtx-initrd', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/release')
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    args = parser.parse_args()
    if args.accel == 'kvm' and subprocess.run(['pgrep', '-x', 'vmware-vmx']).returncode == 0:
        parser.error('Workstation is active')
    output = args.output.resolve()
    output.mkdir(parents=True)
    proof = {name: exercise(output / name, image, args.build, name == 'dell', args.accel)
             for name, image in (('dell', args.dell_initrd), ('rtx', args.rtx_initrd))}
    (output / 'verdict.json').write_text(json.dumps(proof, indent=2) + '\n')
    print('KCONSOLE_CLOSE_QEMU_PASS')


if __name__ == '__main__':
    main()
