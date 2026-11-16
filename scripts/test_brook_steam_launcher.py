#!/usr/bin/env python3
"""Host test for the Dell Steam launcher's ownership and cleanup.

Headless: a private Xvfb display and private PulseAudio, never the host
desktop. A fake Steam tree stands in for SteamRT: its "client" maps a window
with WM_CLASS steam, checks the launcher's Pulse server with pactl, and starts
a descendant in its own session that ignores SIGTERM.

    nix-shell -p xvfb pulseaudio --run 'python3 scripts/test_brook_steam_launcher.py'

Skips (exit 0) if Xvfb, pulseaudio or pactl is missing.
"""
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest

LAUNCHER = Path(__file__).resolve().parent / 'workloads/brook_steam_launcher.py'

FAKE_BWRAP = '''#!{python}
import os, sys
rest = sys.argv[sys.argv.index('--') + 1:]
rest[0] = sys.executable if rest[0] == '/nix/profile/bin/python3' else rest[0]
os.execv(rest[0], rest)
'''

FAKE_HELPER = '''#!{python}
import argparse, os, subprocess, sys
parser = argparse.ArgumentParser()
parser.add_argument('--client'); parser.add_argument('--display'); parser.add_argument('--pulse-server')
args, _ = parser.parse_known_args()
open({record!r} + '/helper.json', 'w').write(__import__('json').dumps(dict(
    argv=sys.argv, display=args.display, pulse=args.pulse_server,
    software=os.environ.get('LIBGL_ALWAYS_SOFTWARE'))))
env = dict(os.environ, DISPLAY=args.display, PULSE_SERVER=args.pulse_server or '')
steam = args.client + '/steamrt64/steam'
sys.exit(subprocess.call([steam, {fake_steam!r}], executable=sys.executable, env=env))
'''

FAKE_STEAM = '''
import os, signal, socket, struct, subprocess, sys, time
sys.path.insert(0, {launcher_dir!r})
from brook_steam_launcher import XConnection
record, mode = {record!r}, {mode!r}
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))  # Steam: normal shutdown
x = XConnection(os.environ['DISPLAY'])
def send(opcode, detail, body):
    body += b'\\0' * (-len(body) % 4)
    x.sock.sendall(struct.pack('<BBH', opcode, detail, 1 + len(body) // 4) + body)
def sync():
    x.request(43, 0)  # GetInputFocus round trip
wid = x.resource_base | 1
send(1, 0, struct.pack('<IIhhHHHHII', wid, x.root, 10, 10, 200, 100, 0, 1, 0, 0))
cls = b'steam\\0Steam\\0'
send(18, 0, struct.pack('<IIIB3xI', wid, 67, 31, 8, len(cls)) + cls)
send(8, 0, struct.pack('<I', wid)); sync()
pactl = subprocess.run(['pactl', 'info'], capture_output=True, text=True)
open(record + '/pactl.txt', 'w').write(pactl.stdout + pactl.stderr)
stubborn = subprocess.Popen([{python!r}, '-c',
    'import os,signal,time; os.setsid(); signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(600)'])
open(record + '/stubborn.pid', 'w').write(str(stubborn.pid))
if mode == 'tray':
    time.sleep(3)
    send(10, 0, struct.pack('<I', wid)); sync()  # close to "tray": no window, still running
while True:
    time.sleep(1)
'''


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    try:
        with open(f'/proc/{pid}/status') as status:
            return not any(line.startswith('State:') and 'Z' in line for line in status)
    except OSError:
        return False


@unittest.skipUnless(all(shutil.which(t) for t in ('Xvfb', 'pulseaudio', 'pactl')), 'needs Xvfb, pulseaudio, pactl')
class LauncherTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.number = next(n for n in range(87, 120) if not os.path.exists(f'/tmp/.X11-unix/X{n}'))
        cls.display = f':{cls.number}'
        cls.xvfb = subprocess.Popen(['Xvfb', cls.display, '-nolisten', 'tcp', '-screen', '0', '640x480x24'],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 10
        while not os.path.exists(f'/tmp/.X11-unix/X{cls.number}'):
            assert time.monotonic() < deadline and cls.xvfb.poll() is None, 'Xvfb did not start'
            time.sleep(0.1)
        time.sleep(0.3)

    @classmethod
    def tearDownClass(cls):
        cls.xvfb.terminate()
        cls.xvfb.wait(5)

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='brook-steam-test-'))
        self.addCleanup(shutil.rmtree, self.root, True)
        home = self.root / 'home'
        client = home / 'steam-20261022/client'
        bwrap = client / 'steamrt64/pv-runtime/steam-runtime-steamrt/pressure-vessel/libexec/steam-runtime-tools-0/srt-bwrap'
        provider = home / '.local/share/brook/steam-provider-20261103'
        for directory in (bwrap.parent, provider, self.root / 'record', self.root / 'run'):
            directory.mkdir(parents=True)
        self.record = self.root / 'record'
        bwrap.write_text(FAKE_BWRAP.format(python=sys.executable))
        (client / 'steamrt64/steam').write_text('#!/bin/false\n')
        (provider / 'ld-linux-x86-64.so.2').write_text('')
        (provider / 'ld.so.cache').write_text('')
        (self.root / 'resolv.conf').write_text('')
        for path in (bwrap, client / 'steamrt64/steam', provider / 'ld-linux-x86-64.so.2'):
            path.chmod(0o755)
        self.env = dict(
            {k: v for k, v in os.environ.items() if k not in ('DISPLAY', 'WAYLAND_DISPLAY', 'PULSE_SERVER', 'DBUS_SESSION_BUS_ADDRESS')},
            BROOK_STEAM_USER_HOME=str(home), BROOK_STEAM_RESOLV=str(self.root / 'resolv.conf'),
            XDG_RUNTIME_DIR=str(self.root / 'run'), DISPLAY=self.display)
        self.home = home

    def helper(self, mode):
        steam = self.root / 'fake_steam.py'
        # sys.executable inside the fake client is its argv[0] (".../steamrt64/steam").
        steam.write_text(FAKE_STEAM.format(launcher_dir=str(LAUNCHER.parent), record=str(self.record), mode=mode,
                                           python=sys.executable))
        helper = self.root / 'helper.py'
        helper.write_text(FAKE_HELPER.format(python=sys.executable, record=str(self.record), fake_steam=str(steam)))
        helper.chmod(0o755)
        self.env['BROOK_STEAM_HELPER'] = str(helper)

    def launch(self, *args):
        return subprocess.Popen([sys.executable, str(LAUNCHER), *args], env=self.env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def log(self):
        path = self.home / '.local/share/brook/steam-launcher.log'
        return path.read_text() if path.exists() else ''

    def wait_log(self, text, seconds):
        deadline = time.monotonic() + seconds
        while text not in self.log():
            self.assertLess(time.monotonic(), deadline, f'{text!r} not logged:\n{self.log()[-3000:]}')
            time.sleep(0.2)

    def assert_clean(self, launcher):
        stubborn = int((self.record / 'stubborn.pid').read_text())
        self.assertFalse(alive(stubborn), 'SIGTERM-ignoring descendant survived')
        self.assertEqual(list((self.root / 'run').iterdir()), [], 'Pulse runtime directory left behind')
        self.assertFalse((self.home / '.local/share/brook/steam-launcher.pid').exists())
        state = json.loads((self.home / '.local/share/brook/steam-launcher-state.json').read_text())
        self.assertEqual(state['state'], 'stopped')
        self.assertEqual(state['leftovers'], [])
        self.assertIn('Server Name: pulseaudio', (self.record / 'pactl.txt').read_text())
        helper = json.loads((self.record / 'helper.json').read_text())
        self.assertEqual(helper['display'], self.display)
        self.assertTrue(helper['pulse'].startswith('unix:' + str(self.root / 'run')))
        self.assertIsNone(helper['software'])  # rootless GPU mode: no forced llvmpipe

    def test_check_reports_display(self):
        self.helper('stay')
        result = subprocess.run([sys.executable, str(LAUNCHER), '--check', '--allow-no-glx'],
                                env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout[:result.stdout.rindex('}') + 1])
        self.assertEqual(report['display']['display'], self.display)
        self.assertIn('GLX', report['display']['extensions'])

    def test_windowless_steam_is_ended(self):
        self.helper('tray')
        started = time.monotonic()
        launcher = self.launch('--windowless-grace', '3', '--startup-timeout', '60')
        out, err = launcher.communicate(timeout=60)
        self.assertEqual(launcher.returncode, 0, err + self.log()[-3000:])
        self.assertIn('first steam window', self.log())
        self.assertIn('no Steam window for 3 s', self.log())
        self.assert_clean(launcher)
        self.assertIn('killing leftover client processes', self.log())
        self.assertLess(time.monotonic() - started, 40)

    def test_sigterm_and_single_instance(self):
        self.helper('stay')
        launcher = self.launch('--windowless-grace', '3')
        self.addCleanup(lambda: launcher.poll() is None and launcher.kill())
        self.wait_log('first steam window', 30)
        second = self.launch()
        second.communicate(timeout=20)
        self.assertEqual(second.returncode, 0)
        self.assertIn('already running', self.log())
        time.sleep(4)  # the window stays: no windowless shutdown
        self.assertIsNone(launcher.poll())
        stopped = time.monotonic()
        launcher.send_signal(signal.SIGTERM)
        launcher.communicate(timeout=40)
        self.assertEqual(launcher.returncode, 0, self.log()[-3000:])
        self.assertLess(time.monotonic() - stopped, 30)
        self.assertIn('signal SIGTERM', self.log())
        self.assert_clean(launcher)


if __name__ == '__main__':
    unittest.main()
