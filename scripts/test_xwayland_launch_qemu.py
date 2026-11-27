#!/usr/bin/env python3
"""Launch feedback for an X11 app on the desktop's shared rootless Xwayland.

Installs a user desktop entry whose program (bash -> exec /boot/bin/XPROBE,
scripts/workloads/x11_input_probe.c) is an X11 client, launches it through the
guest's Apps launcher (hw_launch) and checks the launch observes its window:
the "Starting: <app>" taskbar entry and busy cursor end. Steam reaches the
desktop the same way. Before the fix the X window (owned by waylandd) never
matched the launch, so the launch stayed "waiting_for_window" until the
5-minute timeout. Uses the fixture and set handling of
test_xwayland_input_qemu.py; writes verdict.json, exits non-zero on failure.
"""
import argparse
import json
from pathlib import Path
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'scripts'), str(ROOT / 'tools'), str(ROOT / 'tools/vm_bridge')]
import test_usb_system_qemu as fixture  # noqa: E402
from test_xwayland_input_qemu import prepare_files  # noqa: E402

TITLE = 'XWL Launch Test'
SETUP = r'''set -eu
mkdir -p /data/home/.local/share/applications
printf '%s\n' '#!/nix/profile/bin/bash' 'exec /boot/bin/XPROBE --seconds 240 --dialog-delay 999' > /data/home/xwl-launch.sh
chmod 755 /data/home/xwl-launch.sh
printf '%s\n' '[Desktop Entry]' 'Type=Application' 'Name=XWL Launch Test' \
  'Exec=bash /data/home/xwl-launch.sh' 'Categories=Utility;' \
  > /data/home/.local/share/applications/xwl-launch-test.desktop
HOME=/data/home /nix/profile/bin/brook-apps-index
cat /data/home/.local/share/brook/user-apps.idx
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('files', 'store', 'operator-seed', 'output', 'probe'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--waylandd', type=Path)
    parser.add_argument('--xwm', type=Path)
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    args = parser.parse_args()
    args.output.mkdir(mode=0o700, parents=True)
    args.files = prepare_files(args)
    fixture.adopt_prebuilt(args.files)
    system, fallback = fixture.make_disks(args)
    result = {k: str(getattr(args, k) or 'set') for k in ('kernel', 'waylandd', 'xwm')}

    def write():
        (args.output / 'verdict.json').write_text(json.dumps(result, indent=2) + '\n')

    guest = fixture.Guest(args, system, fallback, 'run')
    try:
        guest.wait_log('USB_SYSTEM_READY', timeout=240)
        guest.attach()
        setup_id = uuid.uuid4().hex
        job = guest.call('hw_guest_run', request_id=setup_id, uid=1000, cwd='/tmp',
                         argv=['/nix/profile/bin/bash', '-c', SETUP], wait_s=4)
        deadline = time.monotonic() + 600  # brook-apps-index scans the profile's icon themes
        while job.get('state') != 'completed' and time.monotonic() < deadline:
            # Short polls: a held session lock starves the script's staged upload.
            time.sleep(2)
            job = guest.call('hw_job', operation_id=setup_id, wait_s=0)
        result['setup'] = {'state': job.get('state'), 'exit': job.get('exit'),
                           'stdout': job.get('stdout', {}).get('text', '')[-800:],
                           'stderr': job.get('stderr', {}).get('text', '')[-800:]}
        write()
        app = None
        for _ in range(30):
            apps = guest.call('hw_apps')
            rows = apps.get('apps') or apps.get('items') or []
            match = [a for a in rows if a.get('title') == TITLE or a.get('name') == TITLE]
            if match:
                app = match[0]
                break
            time.sleep(1)
        if not app:
            result['apps'] = [a.get('title') or a.get('name') for a in rows]
            raise RuntimeError('launcher entry did not appear')
        reference = app.get('reference') or app.get('app')
        result['app'] = reference
        launch_id = uuid.uuid4().hex
        started = time.monotonic()
        status = guest.call('hw_launch', app=reference, request_id=launch_id, wait_for='window', wait_s=5)
        while time.monotonic() - started < 90 and not status.get('wait_condition_met') and \
                status.get('state') not in ('failed', 'rejected', 'lost'):
            status = guest.call('hw_ui', operation_id=launch_id, wait_s=5, wait_for='window')
        result['launch_seconds'] = round(time.monotonic() - started, 1)
        result['launch'] = {k: status.get(k) for k in ('state', 'effect', 'stage', 'wait_condition_met', 'guest', 'error')}
        windows = [w.get('title') for w in guest.call('hw_windows')['windows']]
        result['windows'] = windows
        result['ok'] = bool(status.get('wait_condition_met')) and 'xwl-probe-a' in windows
        write()
    finally:
        write()
        guest.stop()
        for disk in (system, fallback):
            disk.unlink(missing_ok=True)
    print(json.dumps({k: result.get(k) for k in ('launch_seconds', 'launch', 'ok')}, indent=1))
    if not result.get('ok'):
        raise SystemExit('XWAYLAND_LAUNCH_QEMU_FAIL')
    print('XWAYLAND_LAUNCH_QEMU_PASS')


if __name__ == '__main__':
    main()
