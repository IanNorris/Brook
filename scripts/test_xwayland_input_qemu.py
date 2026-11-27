#!/usr/bin/env python3
"""Rootless Xwayland input and cursor proof on a disposable QEMU desktop.

Boots an RTX desktop file set (copied, never modified in place) on a new
disposable BROOK_SYSTEM fixture built from a 128 GiB store image, then runs
scripts/workloads/x11_input_probe.c as uid 1000 on the desktop's Xwayland.
The probe opens two overlapping X11 top-levels and maps a third, created
earlier, after a delay (a toolkit such as Steam/CEF does this for dialogs).

The copied set can carry a different waylandd (--waylandd, /boot/bin) and
brook-xwm (--xwm, /boot/bin, passed to waylandd with --xwm). The QEMU copy's
usb-system record is rewritten to the store image's 128 GiB extent.

Checks, through QEMU's USB mouse and keyboard: each window gets the click
aimed at it and the key typed after that click; the cursor X reports over
the X windows (XFixes) is not the server's default "X" glyph; a depth-24
window whose pixels carry alpha 0x40 shows opaque in a screendump.
Writes verdict.json in --output; exits non-zero on failure. Run in nix-shell.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'scripts'), str(ROOT / 'tools'), str(ROOT / 'tools/vm_bridge')]
import test_usb_system_qemu as fixture  # noqa: E402
from build_rtx_smoke_files import stick_set  # noqa: E402
from window_crop import window_rect  # noqa: E402

PROBE_TITLES = ('xwl-probe-a', 'xwl-probe-b', 'xwl-probe-dialog')
STEP_KEYS = 'qwertyuiop'  # one distinct key per click step


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def mtype(image, name):
    return subprocess.run(['mtype', '-i', image, '::' + name], check=True, capture_output=True).stdout


def mput(image, data, name, scratch):
    source = scratch / ('put-' + name.replace('/', '_'))
    source.write_bytes(data)
    subprocess.run(['mcopy', '-o', '-i', image, source, '::' + name], check=True)


def ppm_pixel(path, x, y):
    with open(path, 'rb') as image:
        assert image.readline().strip() == b'P6'
        width, _ = (int(v) for v in image.readline().split())
        assert image.readline().strip() == b'255'
        image.seek((y * width + x) * 3, 1)
        r, g, b = image.read(3)
    return r << 16 | g << 8 | b


def prepare_files(args, extra=()):
    files = args.output / 'files'
    shutil.copytree(args.files, files)
    initrd = files / 'INITRD.IMG'
    scratch = args.output / 'scratch'
    scratch.mkdir()
    manifest = json.loads((files / 'manifest.json').read_text())
    rc = mtype(initrd, 'BOOT/INIT.RC').decode()
    if getattr(args, 'kernel', None):
        shutil.copyfile(args.kernel, files / 'KERNEL/BROOK.ELF')
        manifest['files']['KERNEL/BROOK.ELF'] = {'bytes': args.kernel.stat().st_size, 'sha256': sha(args.kernel)}
    if args.waylandd:
        mput(initrd, args.waylandd.read_bytes(), 'BOOT/BIN/WAYLANDD', scratch)
    if args.xwm:
        mput(initrd, args.xwm.read_bytes(), 'BOOT/BIN/BROOKXWM', scratch)
        line = 'run --service --once --console /boot/bin/WAYLANDD'
        assert line + '\n' in rc, rc
        rc = rc.replace(line + '\n', line + ' --xwayland /nix/profile/bin/Xwayland --xwm /boot/bin/BROOKXWM\n')
    mput(initrd, args.probe.read_bytes(), 'BOOT/BIN/XPROBE', scratch)
    for name, data in extra:  # small helper files for a test's guest jobs
        mput(initrd, data, name, scratch)
    mput(initrd, rc.encode(), 'BOOT/INIT.RC', scratch)
    record = mtype(initrd, 'ETC/usb-system').decode().split('\n')
    first = int(record[2].split(',')[0])
    record[2] = f'{first},{args.store.stat().st_size // 512}'
    record = '\n'.join(record)
    mput(initrd, record.encode(), 'ETC/usb-system', scratch)
    manifest['init_rc'] = rc
    manifest['usb_system']['record'] = record
    manifest['payload_id'] = manifest['payload_id'][:40] + '-xwlqa'
    (files / 'SET.BIN').write_bytes(stick_set((files / 'KERNEL/BROOK.ELF').read_bytes(),
                                              initrd.read_bytes(), manifest['payload_id']))
    for name in ('INITRD.IMG', 'SET.BIN'):
        manifest['files'][name] = {'bytes': (files / name).stat().st_size, 'sha256': sha(files / name)}
    (files / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    shutil.rmtree(scratch)
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('files', 'store', 'operator-seed', 'output', 'probe'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--waylandd', type=Path)
    parser.add_argument('--xwm', type=Path)
    parser.add_argument('--kernel', type=Path, help='replace KERNEL/BROOK.ELF in the copied set')
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('--dialog-delay', type=int, default=45)
    parser.add_argument('--test-output', metavar='WxH+X+Y',
                        help="waylandd's test-only virtual second output for Xwayland (/etc/waylandd-test-output)")
    parser.add_argument('--trace', action='store_true', help='/etc/xwayland-input-trace (waylandd and brook-xwm input tracing)')
    parser.add_argument('--popups', type=int, default=0,
                        help='the probe first maps and destroys this many tiny override-redirect popups')
    args = parser.parse_args()
    args.output.mkdir(mode=0o700, parents=True)
    extra = []
    if args.test_output:
        extra.append(('ETC/waylandd-test-output', args.test_output.encode() + b'\n'))
    if args.trace:
        extra.append(('ETC/xwayland-input-trace', b'1\n'))
    args.files = prepare_files(args, extra)
    fixture.adopt_prebuilt(args.files)
    system, fallback = fixture.make_disks(args)
    result = {'waylandd': str(args.waylandd) if args.waylandd else 'set',
              'xwm': str(args.xwm) if args.xwm else 'profile'}

    def write():
        (args.output / 'verdict.json').write_text(json.dumps(result, indent=2) + '\n')

    guest = fixture.Guest(args, system, fallback, 'run')
    try:
        guest.wait_log('USB_SYSTEM_READY', timeout=240)
        guest.attach()
        qmp = fixture.Qmp(guest.sock)
        probe_id = uuid.uuid4().hex
        job = guest.call('hw_guest_run', request_id=probe_id, uid=1000, cwd='/tmp',
                         env={'DISPLAY': ':0', 'HOME': '/data/home'},
                         argv=['/boot/bin/XPROBE', '--seconds', '170', '--dialog-delay', str(args.dialog_delay),
                               '--popups', str(args.popups)],
                         wait_s=2)
        result['probe_job'] = job.get('state')

        def windows():
            rows = guest.call('hw_windows')['windows']
            return {row['title']: row for row in rows if row.get('title') in PROBE_TITLES}

        def wait_windows(titles, timeout=60):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                rows = windows()
                if all(t in rows for t in titles):
                    return rows
                time.sleep(1)
            raise RuntimeError(f'windows {titles} not shown: {sorted(windows())}')

        # Real QEMU USB mouse and keyboard (not the kernel's debug injector),
        # the same input path as the desktop's physical devices.
        qmp.call('device_add', driver='usb-mouse', bus='usb.0', port='4.3', id='xwl-mouse')
        guest.wait_log('0627:0001 mouse (slot', timeout=20)

        def pointer():
            reply = guest.call('hw_command', command='mouse')['reply']
            match = re.fullmatch(r'mouse: avail=1 x=(\d+) y=(\d+) btn=0x0+', reply.strip())
            assert match, reply
            return int(match[1]), int(match[2])

        def move_to(x, y):
            for _ in range(200):
                px, py = pointer()
                dx, dy = x - px, y - py
                if not dx and not dy:
                    return
                qmp.call('input-send-event', events=[
                    {'type': 'rel', 'data': {'axis': 'x', 'value': max(-100, min(100, dx))}},
                    {'type': 'rel', 'data': {'axis': 'y', 'value': max(-100, min(100, dy))}}])
                time.sleep(.05)
            raise RuntimeError(f'pointer did not reach {x},{y}')

        def click_type(row, dx, dy, text):
            x, y, w, h = window_rect(row, 'client')
            px, py = x + (dx if dx >= 0 else w + dx), y + (dy if dy >= 0 else h + dy)
            move_to(px, py)
            time.sleep(.3)
            for down in (True, False):
                qmp.call('input-send-event', events=[{'type': 'btn', 'data': {'button': 'left', 'down': down}}])
                time.sleep(.1)
            time.sleep(.6)
            for down in (True, False):
                qmp.call('input-send-event', events=[{'type': 'key', 'data': {
                    'down': down, 'key': {'type': 'qcode', 'data': text}}}])
                time.sleep(.1)
            time.sleep(.4)
            state = {t: {k: v for k, v in r.items() if k in ('focused', 'x', 'y', 'width', 'height')}
                     for t, r in windows().items()}
            return [px, py, state]

        if args.popups:  # let the popup storm finish before the input steps
            deadline = time.monotonic() + 300
            while time.monotonic() < deadline:
                time.sleep(3)
                seen = guest.call('hw_job', operation_id=probe_id, wait_s=0, output_limit=16384)
                if 'POPUPS' in seen.get('stdout', {}).get('text', ''):
                    break
        rows = wait_windows(PROBE_TITLES[:2])
        result['windows_initial'] = {t: window_rect(r, 'client') for t, r in rows.items()}
        steps = []
        # B was mapped last and overlaps A natively; A's top-left is uncovered.
        steps.append(('xwl-probe-b', click_type(rows['xwl-probe-b'], -60, -60, STEP_KEYS[len(steps)])))
        steps.append(('xwl-probe-a', click_type(rows['xwl-probe-a'], 40, 40, STEP_KEYS[len(steps)])))
        steps.append(('xwl-probe-b', click_type(rows['xwl-probe-b'], -60, -60, STEP_KEYS[len(steps)])))
        steps.append(('xwl-probe-a', click_type(rows['xwl-probe-a'], 40, 40, STEP_KEYS[len(steps)])))
        rows = wait_windows(PROBE_TITLES, timeout=args.dialog_delay + 60)
        time.sleep(2)
        result['windows_dialog'] = {t: window_rect(r, 'client') for t, r in rows.items()}
        steps.append(('xwl-probe-dialog', click_type(rows['xwl-probe-dialog'], 30, 30, STEP_KEYS[len(steps)])))
        steps.append(('xwl-probe-b', click_type(rows['xwl-probe-b'], -60, -60, STEP_KEYS[len(steps)])))
        steps.append(('xwl-probe-dialog', click_type(rows['xwl-probe-dialog'], 30, 30, STEP_KEYS[len(steps)])))
        steps.append(('xwl-probe-a', click_type(rows['xwl-probe-a'], 40, 40, STEP_KEYS[len(steps)])))
        result['steps'] = steps
        # Cursor: hover A and B (no X client defines a cursor).
        for name in ('xwl-probe-a', 'xwl-probe-b'):
            x, y, w, h = window_rect(rows[name], 'client')
            px, py = (x + 40, y + 40) if name == 'xwl-probe-a' else (x + w - 60, y + h - 60)
            move_to(px - 3, py - 3)
            move_to(px, py)
            time.sleep(1.5)
            shot = args.output / f'cursor-{name}.ppm'
            qmp.call('screendump', filename=str(shot))
            result.setdefault('cursor_shots', {})[name] = {'file': shot.name, 'pointer': [px, py]}
        # Depth-24 window A holds pixels 0x40c04040 (alpha byte 0x40): it must
        # appear exactly 0xc04040, not blended with the wallpaper.
        x, y, w, h = window_rect(rows['xwl-probe-a'], 'client')
        samples = [ppm_pixel(args.output / 'cursor-xwl-probe-b.ppm', x + 10 + i * 7, y + h - 12) for i in range(4)]
        result['depth24_alpha'] = {'samples': [f'{v:06x}' for v in samples],
                                   'ok': all(v == 0xc04040 for v in samples)}
        write()
        out, err, deadline = '', '', time.monotonic() + 240
        while True:
            job = guest.call('hw_job', operation_id=probe_id, wait_s=4, output_limit=16384,
                             stdout_offset=len(out.encode()), stderr_offset=len(err.encode()))
            out += job.get('stdout', {}).get('text', '')
            err += job.get('stderr', {}).get('text', '')
            if (job.get('state') == 'completed' and job.get('stdout', {}).get('eof')) or time.monotonic() > deadline:
                break
        (args.output / 'probe.out').write_text(out)
        (args.output / 'probe.err').write_text(err)
        result['probe_exit'] = job.get('exit')
        events = [line.split() for line in out.splitlines() if line.startswith('EV ')]
        # Step i: its own key (STEP_KEYS[i]) must reach the target, and the
        # last ButtonPress since the previous step's key must be on the target.
        verdict, begin = [], 0
        for i, (target, point) in enumerate(steps):
            key_at = next((n for n in range(begin, len(events))
                           if events[n][2] == 'KeyPress' and events[n][3] == STEP_KEYS[i]), None)
            window = events[key_at][1] if key_at is not None else None
            stop = key_at if key_at is not None else len(events)
            buttons = [e[1] for e in events[begin:stop] if e[2] == 'ButtonPress']
            begin = stop + 1 if key_at is not None else begin
            verdict.append({'target': target, 'point': point[:2], 'button_windows': buttons,
                            'key_window': window,
                            'ok': buttons == [target] and window == target})
        # Cursor over each X window (probe's XFixes read on EnterNotify). The
        # server's default root cursor is the 16x16 core "X" glyph, hot 7,7.
        cursors = sorted({' '.join(line.split()[2:4]) for line in out.splitlines() if line.startswith('CURSOR ')})
        result['cursor'] = {'seen': cursors,
                            'ok': bool(cursors) and all(not c.startswith('16x16 hot=7,7') for c in cursors)}
        result['input'] = verdict
        result['input_ok'] = all(v['ok'] for v in verdict)
        write()
        log = guest.call('hw_logs', limit=64)
        (args.output / 'guest-log.json').write_text(json.dumps(log, indent=1))
    finally:
        write()
        guest.stop()
        for disk in (system, fallback):
            disk.unlink(missing_ok=True)
    serial = (args.output / 'run' / 'serial.log').read_text(errors='replace')
    probe_out = (args.output / 'probe.out').read_text() if (args.output / 'probe.out').exists() else ''
    root = next((line.split()[1] for line in probe_out.splitlines() if line.startswith('ROOT ')), None)
    result['x_root'] = root
    if args.test_output:
        w, rest = args.test_output.split('x', 1)
        h, x = rest.split('+')[0], rest.split('+')[1]
        result['x_root_expected'] = f'{3840 + int(w) if int(x) == 3840 else None}x{max(2160, int(h))}'
        result['x_root_ok'] = root == result['x_root_expected']
    result['trace_lines'] = [l for l in serial.splitlines() if '] trace' in l][:40]
    result['xwayland'] = {
        'retired': [l for l in serial.splitlines() if 'generation retired' in l or "Wayland connection closed" in l],
        'dropped': [l for l in serial.splitlines() if 'dropped:' in l][:20],
        'popups_done': 'POPUPS' in (args.output / 'probe.out').read_text() if (args.output / 'probe.out').exists() else None}
    write()
    print(json.dumps({k: result.get(k) for k in ('input_ok', 'input', 'cursor', 'depth24_alpha', 'xwayland', 'x_root', 'x_root_ok')}, indent=1))
    if result['xwayland']['retired'] or result.get('x_root_ok') is False or not (result.get('input_ok') and result.get('cursor', {}).get('ok') and
                                             result.get('depth24_alpha', {}).get('ok')):
        raise SystemExit('XWAYLAND_INPUT_QEMU_FAIL')
    print('XWAYLAND_INPUT_QEMU_PASS')
    print(args.output / 'verdict.json')


if __name__ == '__main__':
    main()
