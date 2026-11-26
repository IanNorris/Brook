#!/usr/bin/env python3
"""Apply a signed games profile to a disposable BROOK_SYSTEM and try it.

QEMU here has no GPU 3D, like the RTX desktop today, so this checks the
llvmpipe paths: launcher index rows, each launcher's --check, one Quake II and
one Quake III timedemo, and Steam's prerequisite and display checks (offline:
the fixture has no network, so Steam itself is not started). A screendump is
taken while each game runs. Writes verdict.json in --output.
"""
import argparse
import json
from pathlib import Path
import threading
import time
import uuid

import rtx_smoke_update as rtx
import test_usb_system_qemu as fixture
from test_rtx_smoke_qemu import Qmp
from hardware import Hardware
from hardware_update_workflow import UpdateRun

LAUNCHERS = ('brook-quake2', 'brook-quake3', 'brook-hardware-steam')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('files', 'store', 'operator-seed', 'output', 'plan', 'public-key'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('--bench', action='append', default=None,
                        help='GAME:RENDERER timedemo (default q2:gl3 q2:gl1 q3:opengl1)')
    args = parser.parse_args()
    benches = args.bench or ['q2:gl3', 'q2:gl1', 'q3:opengl1']
    args.profile_transfer = True
    args.boot_set_manifest = args.files / 'manifest.json'
    args.repair = []
    args.fixed_port = fixture.available_port()
    args.output.mkdir(mode=0o700, parents=True)
    fixture.adopt_prebuilt(args.files)
    system, fallback = fixture.make_disks(args)
    result = {}

    def boot(case):
        guest = fixture.Guest(args, system, fallback, case)
        try:
            guest.wait_log('USB_SYSTEM_READY')
            guest.attach()
            return guest, Hardware(guest.base / 'hardware-registry')
        except BaseException:
            guest.stop()
            raise

    def run(guest, bridge, argv, uid=1000, timeout=120):
        job = uuid.uuid4().hex
        bridge.guest_run(guest.session, job, argv, cwd='/tmp', uid=uid, wait_s=2)
        reply = UpdateRun(bridge, args.output).wait_job(
            guest.session, job, lambda row: row.get('state') == 'completed', timeout=timeout,
            output_limit=16384)
        return reply['exit'], reply['stdout']['text'], reply['stderr']['text']

    def write():
        (args.output / 'verdict.json').write_text(json.dumps(result, indent=2) + '\n')

    guest, bridge = boot('apply')
    try:
        init = rtx.Run(bridge, args.output / 'bootstrap').profile_bootstrap(guest.session)
        assert init['state'] == 'profile_initialized', init
        args.session, args.bulk_port = guest.session, guest.bulk_port
        args.profile_plan, args.rollback_profile, args.profile_timeout_s = args.plan, None, 5400
        operator = rtx.Run(bridge, args.output / 'profile')
        started = time.monotonic()
        outcome = {}

        def apply():
            try:
                outcome['receipt'] = operator.profile(argparse.Namespace(**{**vars(args), 'output': operator.output}))
            except BaseException as error:  # reported below
                outcome['error'] = error
        worker = threading.Thread(target=apply, daemon=True)
        worker.start()
        probe = ('cd /nix/store; echo store=$(ls | wc -l); ls -d *steam* *mesa-26* *quake* 2>/dev/null | tr "\\n" " "; '
                 'echo; for d in .brook-nar-* .*tmp* ; do [ -e "$d" ] && du -s "$d"; done 2>/dev/null; df -k /nix | tail -1')
        with (args.output / 'apply-progress.log').open('a') as progress:
            while worker.is_alive():
                worker.join(timeout=120)
                if worker.is_alive():
                    try:
                        _, out, err = run(guest, bridge, ['/nix/profile/bin/bash', '-c', probe], uid=0, timeout=60)
                    except Exception as error:  # progress only
                        out, err = '', repr(error)
                    progress.write(f'{time.monotonic() - started:.0f}s {out} {err}\n')
                    progress.flush()
        if 'error' in outcome:
            raise outcome['error']
        receipt = outcome['receipt']
        assert receipt['state'] == 'profile_verified', receipt
        result['apply_seconds'] = round(time.monotonic() - started, 1)
        result['missing_store_paths'] = len(receipt['missing_store_paths'])
        guest.restart()
    finally:
        guest.stop()

    guest, bridge = boot('games')
    try:
        _, index, _ = run(guest, bridge, ['/nix/profile/bin/bash', '-c',
                          'cat "$(find /nix -maxdepth 5 -name applications.idx -path "*current*" | head -1)" '
                          '|| cat /nix/profile/share/applications/applications.idx'])
        rows = {line.split('\t')[4]: line.split('\t') for line in index.splitlines()
                if len(line.split('\t')) >= 5}
        result['launcher_rows'] = {name: rows.get(name) for name in LAUNCHERS}
        for name in ('brook-quake2-soft', 'brook-quake3-soft'):
            code, out, err = run(guest, bridge, [f'/nix/profile/bin/{name}', '--check'])
            result[name + '_check'] = dict(exit=code, out=out.strip(), err=err.strip()[-400:])
        for mode in ('rootless', 'xephyr'):
            code, out, err = run(guest, bridge, ['/nix/profile/bin/bash', '-c',
                                 f'BROOK_STEAM_MODE={mode} /nix/profile/bin/brook-steam-soft --probe'],
                                 timeout=180)
            result[f'steam_{mode}_probe'] = dict(exit=code, out=out.strip()[-3000:], err=err.strip()[-1500:])
        write()
        for bench in benches:
            game, renderer = bench.split(':')
            shot = args.output / f'{game}-{renderer}.ppm'

            def capture():
                time.sleep(45)
                try:
                    Qmp(guest.sock).call('screendump', filename=str(shot))
                except (OSError, RuntimeError):
                    pass
            snapper = threading.Thread(target=capture, daemon=True)
            snapper.start()
            code, out, err = run(guest, bridge, ['/nix/profile/bin/bash', '-c',
                                 f'BROOK_BENCH_TIMEOUT=200 /nix/profile/bin/brook-games-bench {game} {renderer}; '
                                 f'tail -c 4000 "$(ls -t /data/home/.local/share/brook/bench/{game}-* | head -1)"'],
                                 timeout=260)
            snapper.join(timeout=60)
            result[f'bench_{game}_{renderer}'] = dict(exit=code, out=out.strip()[-4500:], err=err.strip()[-800:],
                                                     screendump=shot.name if shot.exists() else None)
            write()
        guest.restart()
    finally:
        guest.stop()
    write()
    print(args.output / 'verdict.json')


if __name__ == '__main__':
    main()
