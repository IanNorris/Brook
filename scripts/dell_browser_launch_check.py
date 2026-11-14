#!/usr/bin/env python3
"""Launch a browser through the normal Applications catalog entry and check it uses the GPU.

Diffs the shared `gpu worker stats` app class around the launch (never resets it), records
the browser processes' command lines and the Wayland provider, then closes the window.

Usage (repo root, BROOK_ROOT = registry): python3 scripts/dell_browser_launch_check.py SESSION OUTDIR Chromium|Firefox [seconds]
"""
import json, sys, time, uuid
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tools/vm_bridge')]
from hardware import Hardware

session, out, title = sys.argv[1], Path(sys.argv[2]), sys.argv[3]
secs = float(sys.argv[4]) if len(sys.argv) > 4 else 20
out.mkdir(parents=True, exist_ok=False)
h = Hardware()
def save(n, v): (out / f'{n}.json').write_text(json.dumps(v, indent=2) + '\n'); return v
def app_class(): return h.command(session, 'gpu worker stats').get('class', {}).get('app', [0, 0])
app = next(a for a in h.apps(session)['apps'] if a['title'] == title)
before_windows = {w['window_id'] for w in h.windows(session)['windows']}
before = save('worker-before', app_class())
launch = save('launch', h.launch(session, app['reference'], uuid.uuid4().hex, wait_for='window', wait_s=20))
end = time.monotonic() + 40
win = None
while time.monotonic() < end and not win:
    new = [w for w in h.windows(session)['windows'] if w['window_id'] not in before_windows]
    win = new[0] if new else None
    if not win: time.sleep(1)
save('window', win)
time.sleep(secs)
after = save('worker-after', app_class())
pid = (win or {}).get('client', {}).get('pid')
cmd = ('for p in /proc/[0-9]*; do c=$(tr "\\0" " " < $p/cmdline 2>/dev/null); case "$c" in '
       '*chromium*|*firefox*) echo "${p#/proc/} $c" | cut -c1-400;; esac; done')
r = h.guest_run(session, uuid.uuid4().hex, ['/nix/profile/bin/bash', '-c', cmd], cwd='/tmp', uid=0, wait_s=15)
procs = save('browser-procs', r['stdout']['text'])
closed = None
if win:
    closed = h.window(session, win['reference'], 'close', uuid.uuid4().hex, wait_s=10).get('state')
time.sleep(8)
left = [w['title'] for w in h.windows(session)['windows'] if w['window_id'] not in before_windows]
result = save('result', {'title': title, 'window': (win or {}).get('title'), 'pid': pid,
                         'app_jobs': after[0] - before[0], 'app_us': after[1] - before[1],
                         'angle_flags': '--use-angle=gles' in procs, 'disable_gpu': '--disable-gpu' in procs, 'closed': closed, 'windows_left': left})
print(json.dumps(result)); print(procs[:1500])
