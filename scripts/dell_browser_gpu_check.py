#!/usr/bin/env python3
"""Check browser GPU rendering on the Dell without deploying anything.

Uploads the GPU browser wrappers (data/nix-wrappers) and the guest probes into current-boot RAM,
then for each requested browser and mode: resets `gpu worker stats`, runs the probe as uid 1000,
and saves the worker stats (the `app` class counts GPU jobs from applications) and probe result.

Usage (repo root, BROOK_ROOT = the registry holding SESSION):
  python3 scripts/dell_browser_gpu_check.py SESSION OUTDIR [chromium,firefox] [gpu,software] [seconds]
"""
import json, sys, tarfile, time, uuid
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tools/vm_bridge')]
from hardware import Hardware
from hardware_bulk_workflow import BulkRun

session, out = sys.argv[1], Path(sys.argv[2])
browsers = (sys.argv[3] if len(sys.argv) > 3 else 'chromium,firefox').split(',')
modes = (sys.argv[4] if len(sys.argv) > 4 else 'gpu,software').split(',')
secs = sys.argv[5] if len(sys.argv) > 5 else '12'
out.mkdir(parents=True, exist_ok=False)
h = Hardware()
KEY = '/data/Programming/Brook/shared/dell/hardware-update-key'
work = '/tmp/brook-browser-gpu-' + uuid.uuid4().hex[:8]

bundle = out / 'bundle.tar'
with tarfile.open(bundle, 'w') as tar:
    for name in ('browser_gpu_probe.py', 'firefox_gpu_probe.py', 'browser_gpu_probe_page.py'):
        tar.add(ROOT / 'scripts/workloads' / name, arcname=name)
    for name in ('brook-chromium', 'brook-firefox'):
        tar.add(ROOT / 'data/nix-wrappers' / name, arcname=name)


def save(name, value):
    (out / f'{name}.json').write_text(json.dumps(value, indent=2) + '\n'); return value


def job(argv, uid=1000, timeout=180, name=None):
    r = h.guest_run(session, uuid.uuid4().hex, argv, cwd='/tmp', uid=uid, wait_s=2, task_limit=200)
    end = time.monotonic() + timeout
    while r['state'] not in ('completed', 'failed') and time.monotonic() < end:
        r = h.job(session, r['operation_id'], wait_s=4, output_limit=16384)
    if name: save(name, r)
    return r


bash = '/nix/profile/bin/bash'
save('doctor', h.doctor(session))
xfer = BulkRun(h, out / 'bundle-transfer').execute(session, KEY, 'put', source=bundle, request_id=uuid.uuid4().hex)
setup = job([bash, '-c', f'set -e; mkdir -p {work}; cd {work}; tar -xf {xfer["transfer"]["guest_path"]}; '
             f'chmod 755 brook-chromium brook-firefox; chown -R 1000 {work}; '
             'ls -la /nix/profile/bin/brook-iris-run; readlink -f /nix/profile/bin/brook-iris-run'], uid=0, name='setup')
print('setup', setup['state'], setup['stdout']['text'].strip()[-400:], setup['stderr']['text'][-400:])
save('providers-before', h.command(session, 'procs'))
for browser in browsers:
    for mode in modes:
        label = f'{browser}-{mode}'
        probe = 'browser_gpu_probe.py' if browser == 'chromium' else 'firefox_gpu_probe.py'
        env = '' if mode == 'gpu' else 'BROOK_BROWSER_SOFTWARE=1 '
        h.command(session, 'gpu worker stats reset')
        r = job([bash, '-c', f'{env}/nix/profile/bin/python3 {work}/{probe} {work}/{label} {work}/brook-{browser} {secs}'],
                name=label + '-job')
        save(label + '-worker', h.command(session, 'gpu worker stats'))
        text = r['stdout']['text']
        line = next((l for l in text.splitlines() if l.startswith('BROWSER_GPU_RESULT ')), None)
        result = json.loads(line.split(' ', 1)[1]) if line else {'error': 'no result', 'stderr': r['stderr']['text'][-2000:]}
        save(label + '-result', result)
        log = job([bash, '-c', f'tail -c 6000 {work}/{label}/browser.log'], name=label + '-log')
        app = json.loads((out / f'{label}-worker.json').read_text()).get('class', {}).get('app', [0])
        print(label, 'exit', r.get('exit'), 'app_jobs', app[0], 'frames', result.get('frames'),
              'status', (result.get('gpu') or {}).get('featureStatus', {}).get('gpu_compositing') or
              ((result.get('gfx') or {}).get('layerManager')), 'error', result.get('error'), flush=True)
save('providers-after', h.command(session, 'procs'))
print('done', out)
