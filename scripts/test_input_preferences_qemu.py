#!/usr/bin/env python3
"""Check USB keyboard layouts, cursor size, and boot timing in headless QEMU."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

from PIL import Image, ImageChops

from test_rtx_smoke_qemu import Qmp

ROOT = Path(__file__).resolve().parents[1]


def run(*args, **kwargs):
    subprocess.run(args, check=True, **kwargs)


def make_application_image(output, provider, client):
    source = output / 'applications'
    store = source / 'store'
    store.mkdir(parents=True)
    closure = subprocess.check_output(['nix-store', '-qR', provider, client], text=True).splitlines()
    for path in sorted(set(closure)):
        shutil.copytree(path, store / Path(path).name, symlinks=True)
    image = output / 'APPS.BRK'
    checksum = ROOT / 'build/release/host_tests/libbrook_image_checksum.so'
    run('python3', ROOT / 'scripts/compressed_image.py', 'build', source, image,
        env=dict(os.environ, BROOK_IMAGE_CHECKSUM_LIBRARY=str(checksum)))
    for parent, _, _ in os.walk(source):
        os.chmod(parent, 0o755)
    shutil.rmtree(source)
    return image


def wait_for(vm, serial, fragment, seconds=60):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if serial.exists():
            log = serial.read_text(errors='replace')
            if fragment in log:
                return log
        if vm.poll() is not None:
            raise RuntimeError(f'QEMU exited early, see {serial}')
        time.sleep(.1)
    raise RuntimeError(f'Guest never logged {fragment!r}, see {serial}')


def send_key(qmp, *keys):
    for down, sequence in ((True, keys), (False, keys[::-1])):
        for code in sequence:
            qmp.call('input-send-event', events=[{'type': 'key',
                     'data': {'key': {'type': 'qcode', 'data': code}, 'down': down}}])
            time.sleep(.07)


def move(qmp, dx, dy):
    while dx or dy:
        x = max(-30, min(30, dx))
        y = max(-30, min(30, dy))
        qmp.call('input-send-event', events=[{'type': 'rel', 'data': {'axis': 'x', 'value': x}},
                                             {'type': 'rel', 'data': {'axis': 'y', 'value': y}}])
        dx -= x
        dy -= y
        time.sleep(.07)


def screenshot(qmp, file):
    qmp.call('screendump', filename=str(file))
    with Image.open(file) as pic:
        return pic.convert('RGB')


def exercise(output, provider, client, image, layout, size, scale, build):
    folder = output / f'{layout}-{size}'
    folder.mkdir()
    probe = folder / 'probe'
    run('python3', ROOT / 'scripts/make_uefi_probe.py', '--build', build, '--output', probe,
        '--display-width', '800', '--display-height', '600', '--volatile-desktop',
        '--driver', 'xhci', '--usb-read-only',
        '--application-image', image, '--media-mount', '/media/usb', '--application-mount', '/nix',
        env=dict(os.environ, BROOK_INITRD_KIB='8192'))
    initrd = probe / 'payload/INITRD.IMG'
    keyboard = folder / 'keyboard-layout'
    keyboard.write_text(layout + '\n')
    config = folder / 'desktop-defaults.ini'
    config.write_text(f'[Desktop]\ncursor_size={size}\nscale={scale}\n')
    rc = folder / 'INIT.RC'
    guest_provider = '/nix/store/' + provider.name + '/bin/waylandd'
    guest_client = '/nix/store/' + client.name + '/bin/input-preferences-fixture'
    fractional = f' {scale}' if scale in (120, 135) else ''
    rc.write_text(f'set wm\nset vfb none\nrun --service --once --uid 1000 {guest_provider}\n'
                  f'run --console --uid 1000 {guest_client} {layout} {size}{fractional}\n')
    for source, dest in ((keyboard, 'ETC/keyboard-layout'), (config, 'ETC/desktop-defaults.ini'), (rc, 'BOOT/INIT.RC')):
        run('mcopy', '-o', '-i', initrd, source, '::' + dest)
    disk = probe / 'probe.raw'
    run('mcopy', '-o', '-i', f'{disk}@@1048576', initrd, '::INITRD.IMG')
    variables = folder / 'VARS.fd'
    shutil.copyfile(os.environ['OVMF_VARS'], variables)
    serial = folder / 'serial.log'
    sock = folder / 'qmp.sock'
    # The socket path for the long nested probe path can exceed AF_UNIX limits.
    short_sock = output / 'qmp.sock'
    if len(os.fsencode(sock)) >= 104:
        sock = short_sock
    kvm = os.access('/dev/kvm', os.R_OK | os.W_OK)
    qemu = ['qemu-system-x86_64', '-machine', 'q35,i8042=off',
            '-accel', 'kvm' if kvm else 'tcg', '-cpu', 'host' if kvm else 'max', '-smp', '4', '-m', '2048',
            '-nodefaults', '-drive', f'if=pflash,format=raw,readonly=on,file={os.environ["OVMF_CODE"]}',
            '-drive', f'if=pflash,format=raw,file={variables}', '-device', 'VGA', '-device', 'qemu-xhci,id=usb',
            '-drive', f'if=none,id=stick,format=raw,readonly=on,file={disk}',
            '-device', 'usb-storage,bus=usb.0,port=1,drive=stick,bootindex=1',
            '-device', 'usb-hub,bus=usb.0,port=2',
            '-device', 'usb-kbd,bus=usb.0,port=2.1,id=kbd',
            '-device', 'usb-mouse,bus=usb.0,port=2.2,id=mouse',
            '-display', 'none', '-nic', 'none', '-serial', f'file:{serial}',
            '-qmp', f'unix:{sock},server=on,wait=off', '-no-reboot']
    with (folder / 'qemu.log').open('w') as log:
        vm = subprocess.Popen(qemu, stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_for(vm, serial, 'INPUT_PREFS_FOCUSED')
            if fractional:
                units = scale * 120 // 100
                width = (256 * units + 60) // 120
                height = (128 * units + 60) // 120
                log = wait_for(vm, serial, f'INPUT_PREFS_SCALE scale120={units} buffer={width}x{height}')
                assert re.search(rf"WM: created window .*'Input preferences fixture'.* {width}x{height} ", log), serial
            if scale != 100:
                wait_for(vm, serial, f'WM: desktop scale applied {scale}%')
            qmp = Qmp(sock)
            # Save both cursor positions with the same static background.
            image_a = screenshot(qmp, folder / 'cursor.ppm')
            move(qmp, 220 if scale>100 else 120, 90)
            wait_for(vm, serial, 'INPUT_PREFS_CLIENT_MOTION', 12)
            qmp.call('input-send-event', events=[{'type': 'rel', 'data': {'axis': 'x', 'value': 0}}])
            wait_for(vm, serial, 'BOOT_POINTER usb_ignored', 8)
            image_b = screenshot(qmp, folder / 'cursor-moved.ppm')
            for chord in [('shift','2'), ('shift','3'), ('shift','apostrophe'), ('backslash',),
                          ('shift','backslash'), ('less',), ('shift','less'), ('alt_r','4'), ('alt_r','grave_accent')]:
                send_key(qmp, *chord)
            wait_for(vm, serial, 'INPUT_PREFS_KEYS_PASS', 12)
            diff = ImageChops.difference(image_a, image_b)
            # With i8042 disabled, the relative USB cursor starts at (0, 0).
            box = diff.crop((0, 0, 128, 128)).getbbox()
            if not box:
                raise RuntimeError('No pointer pixels changed at the initial pointer position')
            log = wait_for(vm, serial, 'BOOT_POINTER wayland_delivered', 8)
            stamps = {}
            usb_identity = None
            for name in ('usb_enumerated', 'usb_queued', 'usb_bound', 'usb_complete', 'usb_report',
                         'usb_rearm', 'usb_ignored', 'desktop', 'cursor', 'ingress', 'wm',
                         'seat_bound', 'wayland_delivered'):
                matches = re.findall(rf'^BOOT_POINTER {name} .*?kernel_ms=(\d+)(.*)$', log, re.MULTILINE)
                if not matches:
                    raise RuntimeError(f'Missing boot trace {name}, see {serial}')
                stamps[name] = int(matches[0][0])
                fields = dict(field.split('=', 1) for field in matches[0][1].split())
                if name.startswith('usb_'):
                    identity = tuple(fields[key] for key in ('controller', 'port', 'vid', 'pid'))
                    if usb_identity is None:
                        usb_identity = identity
                        # QEMU maps the bus port to a controller-specific root port.
                        assert fields['port'].endswith('.2'), fields
                    assert identity == usb_identity, fields
                if name == 'usb_queued':
                    assert fields['boot_protocol'] == 'accepted', fields
                    assert int(fields['esit_us']) > 0, fields
                if name in ('usb_complete', 'usb_report', 'usb_ignored'):
                    assert fields['cc'] in ('1', '13'), fields
                    assert 3 <= int(fields['bytes']) <= int(fields['requested']), fields
                if name == 'usb_ignored':
                    assert fields['reason'] == 'idle', fields
                if name in ('ingress', 'wm'):
                    assert fields['detail'] == '6', fields
            assert stamps['usb_enumerated'] <= stamps['usb_queued'] <= stamps['usb_bound']
            assert stamps['usb_queued'] <= stamps['usb_complete'] <= stamps['usb_rearm']
            assert stamps['usb_complete'] <= stamps['usb_report'] <= stamps['ingress']
            assert stamps['ingress'] <= stamps['wm'] <= stamps['wayland_delivered']
            assert stamps['seat_bound'] <= stamps['wayland_delivered']
            return {'cursor_bbox':box,'cursor_height':box[3]-box[1], 'kernel_ms':stamps}
        finally:
            vm.terminate()
            try:
                vm.wait(timeout=5)
            except subprocess.TimeoutExpired:
                vm.kill()
                vm.wait()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('output','provider','client'):
        ap.add_argument('--'+name,type=Path,required=True)
    ap.add_argument('--build',type=Path,default=ROOT/'build/release')
    ap.add_argument('--scale',type=int,choices=(100,120,135,200),default=100)
    args=ap.parse_args()
    if subprocess.run(['pgrep','-x','vmware-vmx'],stdout=subprocess.DEVNULL).returncode == 0:
        ap.error('Workstation is running')
    output=args.output.resolve()
    output.mkdir(parents=True)
    image=make_application_image(output,args.provider,args.client)
    proof={}
    sizes = (48,64) if args.scale==200 else (24,48)
    for layout,size in zip(('gb','us'), sizes):
        proof[layout]=exercise(output,args.provider,args.client,image,layout,size,args.scale,args.build)
        (output/'verdict.json').write_text(json.dumps(proof,indent=2)+'\n')
    assert proof['us']['cursor_height'] > proof['gb']['cursor_height'], proof
    print('INPUT_PREFERENCES_QEMU_PASS',json.dumps(proof))


if __name__ == '__main__':
    main()
