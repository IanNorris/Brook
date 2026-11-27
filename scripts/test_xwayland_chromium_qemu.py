#!/usr/bin/env python3
"""Two Chromium X11 windows (one process, like Steam's CEF) on the desktop's
rootless Xwayland in a disposable QEMU desktop; check each gets its clicks and keys.

Uses the fixture and set handling of test_xwayland_input_qemu.py. Chromium
runs with --ozone-platform=x11 on DISPLAY :0. Each window shows a data: page
whose title counts mouse presses and records keys, so the native window title
(hw_windows) tells which window received the input. Input comes from QEMU's
USB mouse and keyboard. Writes verdict.json; exits non-zero on failure.
"""
import argparse
import json
from pathlib import Path
import re
import sys
import time
import urllib.parse
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'scripts'), str(ROOT / 'tools'), str(ROOT / 'tools/vm_bridge')]
import test_usb_system_qemu as fixture  # noqa: E402
from test_xwayland_input_qemu import prepare_files  # noqa: E402
from window_crop import window_rect  # noqa: E402

CHROMIUM = '/nix/store/q2mhvi0zha12xj61xpkywxxcg3pi7vm2-chromium-brook-148.0.7778.96/bin/chromium'
KEYS = 'qwertyuiop'


def body(name, colour):
    return ('<title>%s c=0 k=</title><body style="margin:0;background:%s">'
            '<script>let c=0,k="";function t(){document.title="%s c="+c+" k="+k}'
            'onmousedown=()=>{c++;t()};onkeydown=e=>{k+=e.key;t()};</script>') % (name, colour, name)


def page():
    """Window A; on load it opens B as a separate top-level popup window of
    the same Chromium process (as Steam opens its dialogs), written into an
    about:blank document (top-level data: navigation is blocked)."""
    b = body('cw-b', '#46c').replace('\\', '\\\\').replace("'", "\\'")
    html = body('cw-a', '#c44') + (
        "<script>onload=()=>{let w=window.open('','cwb','popup,left=620,top=460,width=700,height=480');"
        "w.document.write('%s');w.document.close();setTimeout(()=>w.moveTo(620,460),3000);}</script>" % b.replace('</script>', '<\\/script>'))
    return 'data:text/html,' + urllib.parse.quote(html)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('files', 'store', 'operator-seed', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True, help='any small file for /boot/bin/XPROBE (unused)')
    parser.add_argument('--waylandd', type=Path)
    parser.add_argument('--xwm', type=Path)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    args = parser.parse_args()
    args.output.mkdir(mode=0o700, parents=True)
    args.files = prepare_files(args)
    fixture.adopt_prebuilt(args.files)
    system, fallback = fixture.make_disks(args)
    result = {'waylandd': str(args.waylandd or 'set'), 'xwm': str(args.xwm or 'profile')}

    def write():
        (args.output / 'verdict.json').write_text(json.dumps(result, indent=2) + '\n')

    guest = fixture.Guest(args, system, fallback, 'run')
    try:
        guest.wait_log('USB_SYSTEM_READY', timeout=240)
        guest.attach()
        qmp = fixture.Qmp(guest.sock)
        common = (f'{CHROMIUM} --ozone-platform=x11 --no-sandbox --disable-gpu --no-first-run '
                  '--user-data-dir=/tmp/xwl-chromium --password-store=basic --disable-features=Translate')
        script = (f'export DISPLAY=:0 HOME=/data/home; unset WAYLAND_DISPLAY; '
                  f'{common} --disable-popup-blocking --window-position=120,140 --window-size=700,500 '
                  f'"{page()}" >/tmp/xwl-chromium.log 2>&1; sleep 400')
        job_id = uuid.uuid4().hex
        guest.call('hw_guest_run', request_id=job_id, uid=1000, cwd='/tmp',
                   argv=['/nix/profile/bin/bash', '-c', script], wait_s=2)

        def windows():
            rows = guest.call('hw_windows')['windows']
            return {row['title'].split(' ')[0]: row for row in rows
                    if row.get('title', '').startswith(('cw-a ', 'cw-b '))}

        deadline = time.monotonic() + 360
        while time.monotonic() < deadline and len(windows()) < 2:
            time.sleep(2)
        rows = windows()
        result['windows'] = {k: [r['title'], window_rect(r, 'client')] for k, r in rows.items()}
        if len(rows) < 2:
            try:
                log = guest.job('tail -c 6000 /tmp/xwl-chromium.log; true')
                result['chromium_log'] = log.get('stdout', {}).get('text', '')
            except (AssertionError, RuntimeError) as error:
                result['chromium_log'] = repr(error)
            result['all_windows'] = [r.get('title') for r in guest.call('hw_windows')['windows']]
            result['job'] = guest.call('hw_job', operation_id=job_id, wait_s=1)
            raise RuntimeError(f'Chromium windows not shown: {result["windows"]}')
        time.sleep(8)
        rows = windows()
        result['windows_settled'] = {k: [r['title'], window_rect(r, 'client')] for k, r in rows.items()}
        qmp.call('device_add', driver='usb-mouse', bus='usb.0', port='4.3', id='xwl-mouse')
        guest.wait_log('0627:0001 mouse (slot', timeout=20)

        def pointer():
            reply = guest.call('hw_command', command='mouse')['reply']
            match = re.fullmatch(r'mouse: avail=1 x=(\d+) y=(\d+) btn=0x0+', reply.strip())
            assert match, reply
            return int(match[1]), int(match[2])

        def move_to(x, y):
            for _ in range(300):
                px, py = pointer()
                dx, dy = x - px, y - py
                if not dx and not dy:
                    return
                qmp.call('input-send-event', events=[
                    {'type': 'rel', 'data': {'axis': 'x', 'value': max(-100, min(100, dx))}},
                    {'type': 'rel', 'data': {'axis': 'y', 'value': max(-100, min(100, dy))}}])
                time.sleep(.05)
            raise RuntimeError(f'pointer did not reach {x},{y}')

        expected = {'cw-a': [0, ''], 'cw-b': [0, '']}
        steps = []
        # Chromium's content starts below its own tab strip/toolbar.
        for i, target in enumerate(['cw-b', 'cw-a', 'cw-b', 'cw-a', 'cw-b', 'cw-a']):
            row = windows()[target]
            x, y, w, h = window_rect(row, 'client')
            px, py = (x + w - 80, y + h - 60) if target == 'cw-b' else (x + 60, y + h - 60)
            move_to(px, py)
            time.sleep(.4)
            for down in (True, False):
                qmp.call('input-send-event', events=[{'type': 'btn', 'data': {'button': 'left', 'down': down}}])
                time.sleep(.15)
            time.sleep(1.0)
            for down in (True, False):
                qmp.call('input-send-event', events=[{'type': 'key', 'data': {
                    'down': down, 'key': {'type': 'qcode', 'data': KEYS[i]}}}])
                time.sleep(.15)
            expected[target][0] += 1
            expected[target][1] += KEYS[i]
            time.sleep(2.5)
            titles = {k: r['title'] for k, r in windows().items()}
            want = {k: f'{k} c={v[0]} k={v[1]}' for k, v in expected.items()}
            ok = all(titles.get(k, '').startswith(want[k]) for k in want)
            steps.append({'target': target, 'point': [px, py], 'titles': titles, 'want': want, 'ok': ok})
            write()
        result['steps'] = steps
        result['input_ok'] = all(s['ok'] for s in steps)
        write()
    finally:
        write()
        guest.stop()
        for disk in (system, fallback):
            disk.unlink(missing_ok=True)
    print(json.dumps(result.get('steps'), indent=1))
    if not result.get('input_ok'):
        raise SystemExit('XWAYLAND_CHROMIUM_QEMU_FAIL')
    print('XWAYLAND_CHROMIUM_QEMU_PASS')


if __name__ == '__main__':
    main()
