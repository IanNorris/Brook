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


def body(name, colour, select=False):
    """Title "<name> c=<presses> k=<keys> s=<selected> x=<clientX> y=<clientY>".
    With select, a large <select> whose dropdown Chromium shows in an
    override-redirect X window (like Steam's menus)."""
    menu = ('<select style="position:fixed;left:20px;top:20px;font-size:48px" onchange="v=this.value;t()">'
            '<option>o1</option><option>o2</option><option>o3</option><option>o4</option></select>'
            if select else '')
    return ('<title>%s c=0 k=</title><body style="margin:0;background:%s">%s'
            '<script>let c=0,k="",v="",x=0,y=0;'
            'function t(){document.title="%s c="+c+" k="+k+" s="+v+" x="+x+" y="+y}'
            'onmousedown=e=>{c++;x=e.clientX;y=e.clientY;t()};onkeydown=e=>{k+=e.key;t()};</script>'
            ) % (name, colour, menu, name)


def page():
    """Window A; on load it opens B as a separate top-level popup window of
    the same Chromium process (as Steam opens its dialogs), written into an
    about:blank document (top-level data: navigation is blocked)."""
    b = body('cw-b', '#46c', select=True).replace('\\', '\\\\').replace("'", "\\'")
    html = body('cw-a', '#c44') + (
        "<script>onload=()=>{let w=window.open('','cwb','popup,left=900,top=460,width=700,height=480');"
        "w.document.write('%s');w.document.close();setTimeout(()=>w.moveTo(900,460),3000);}</script>" % b.replace('</script>', '<\\/script>'))
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
    common = (f'{CHROMIUM} --ozone-platform=x11 --no-sandbox --disable-gpu --no-first-run '
              '--user-data-dir=/tmp/xwl-chromium --password-store=basic --disable-features=Translate')
    # The launch script travels in the set (/boot/bin/XCHROME): a short job argv.
    script = ('#!/nix/profile/bin/bash\nexport DISPLAY=:0 HOME=/data/home; unset WAYLAND_DISPLAY\n'
              f'{common} --disable-popup-blocking --window-position=120,140 --window-size=700,500 '
              f'"{page()}" >/tmp/xwl-chromium.log 2>&1\n')
    args.files = prepare_files(args, [('BOOT/BIN/XCHROME', script.encode())])
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
        job_id = uuid.uuid4().hex
        guest.call('hw_guest_run', request_id=job_id, uid=1000, cwd='/tmp',
                   argv=['/nix/profile/bin/bash', '/boot/bin/XCHROME'], wait_s=2)

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
        # Dropdown (an override-redirect X window) in B, opened by the click
        # that also activates B. Find B's page origin from the last click on
        # B (its title records clientX/clientY), click the <select>, then the
        # third option of the list Chromium opens below it.
        last_b = [st for st in steps if st['target'] == 'cw-b'][-1]
        fields = dict(f.split('=', 1) for f in last_b['titles']['cw-b'].split(' ')[1:] if '=' in f)
        ox, oy = last_b['point'][0] - int(fields['x']), last_b['point'][1] - int(fields['y'])
        row = windows()['cw-a']
        x, y, w, h = window_rect(row, 'client')
        for point in ((x + 60, y + h - 60), (ox + 90, oy + 50), (ox + 90, oy + 20 + 62 + 60 * 2 + 30)):
            move_to(*point)
            time.sleep(.4)
            for down in (True, False):
                qmp.call('input-send-event', events=[{'type': 'btn', 'data': {'button': 'left', 'down': down}}])
                time.sleep(.15)
            time.sleep(2.0)
        title = windows()['cw-b']['title']
        chosen = dict(f.split('=', 1) for f in title.split(' ')[1:] if '=' in f).get('s', '')
        result['dropdown'] = {'origin': [ox, oy], 'title': title, 'selected': chosen,
                              'ok': chosen in ('o2', 'o3', 'o4')}
        result['input_ok'] = all(s['ok'] for s in steps) and result['dropdown']['ok']
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
