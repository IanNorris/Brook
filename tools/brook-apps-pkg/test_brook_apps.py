"""Offline tests for the Brook user app index and web-app builder."""
import importlib.machinery
import importlib.util
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import brook_apps  # noqa: E402


def load_script(name):
    loader = importlib.machinery.SourceFileLoader(name.replace('-', '_'), str(HERE / name))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


webapp = load_script('brook-webapp')


class Env(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.bin = self.root / 'profile-bin'
        self.bin.mkdir()
        for p in ('brook-chromium', 'gimp', 'flatpak'):
            (self.bin / p).write_text('#!/bin/sh\n')
        self.old = dict(os.environ)
        os.environ['XDG_DATA_HOME'] = str(self.root / 'share')
        os.environ['HOME'] = str(self.root)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self.old)
        self.tmp.cleanup()

    def desktop(self, name, body, directory='applications'):
        d = self.root / 'share' / directory
        d.mkdir(parents=True, exist_ok=True)
        (d / name).write_text('[Desktop Entry]\nType=Application\n' + body)
        return d


class Translate(Env):
    def t(self, line):
        return brook_apps.translate_exec(line, self.bin)

    def test_field_codes_and_profile_path(self):
        self.assertEqual(self.t('gimp %U'), str(self.bin / 'gimp'))
        self.assertEqual(self.t('/nix/store/abc-gimp/bin/gimp %f'), str(self.bin / 'gimp'))

    def test_chromium_is_wrapped_and_keeps_app_args(self):
        self.assertEqual(self.t('/opt/chromium --profile-directory=Default --app-id=abc'),
                         f'{self.bin}/brook-chromium --profile-directory=Default --app-id=abc')

    def test_rejections(self):
        for bad in ('notinstalled', '-x gimp', 'env A=1 gimp', 'A=1 gimp', 'gimp --uid 0', 'gimp --uid=0',
                    'gimp "a b"', "gimp it\\'s", 'gimp `x`', 'gimp ' + 'a' * 300, 'gimp "unterminated'):
            self.assertIsNone(self.t(bad), bad)

    def test_urls_keep_query(self):
        self.assertEqual(self.t('brook-chromium --app=https://x.example/?a=1&b=2#f'),
                         f'{self.bin}/brook-chromium --app=https://x.example/?a=1&b=2#f')


class Index(Env):
    def png(self, path, size=48):
        from PIL import Image
        path.parent.mkdir(parents=True, exist_ok=True)
        Image.new('RGBA', (size, size), (255, 0, 0, 128)).save(path)

    def test_build_and_write(self):
        self.desktop('chat.desktop', 'Name=Chat\tApp\nExec=brook-chromium --app=https://chat.example/\n'
                     'Icon=chat\nCategories=Network;Chat;\nStartupWMClass=chrome-chat.example__-Default\n'
                     'X-Brook-WebApp=https://chat.example/\n')
        self.desktop('hidden.desktop', 'Name=H\nExec=gimp\nNoDisplay=true\n')
        self.desktop('bad.desktop', 'Name=Bad\nExec=rm -rf /\n')
        self.desktop('link.desktop', 'Type=Link\nName=L\nExec=gimp\n')
        self.png(self.root / 'share/icons/hicolor/48x48/apps/chat.png')
        self.png(self.root / 'share/icons/hicolor/256x256/apps/chat.png', 256)
        lines, icons, skipped = brook_apps.build_index(profile_bin=self.bin)
        self.assertEqual(lines, [f'Chat App\t{self.bin}/brook-chromium --app=https://chat.example/\t'
                                 'chrome-chat.example__-default.rgba\tNetwork;Chat;X-Brook-WebApp;\tchrome-chat.example__-Default'])
        self.assertEqual([Path(p).name for p, _ in skipped], ['bad.desktop'])
        data = icons['chrome-chat.example__-default.rgba']
        self.assertEqual(len(data), 24 * 24 * 4)
        a, r = struct.unpack('<I', data[:4])[0] >> 24, (struct.unpack('<I', data[12 * 24 * 4 + 48:12 * 24 * 4 + 52])[0] >> 16) & 255
        self.assertEqual(r, 255)
        out = self.root / 'index'
        self.assertTrue(brook_apps.write_index(lines, icons, out))
        self.assertFalse(brook_apps.write_index(lines, icons, out))
        (out / 'icons/stale.rgba').write_bytes(b'x')
        brook_apps.write_index([], {}, out)
        self.assertEqual(list((out / 'icons').iterdir()), [])
        self.assertEqual((out / 'user-apps.idx').read_text(), '')

    def test_user_dir_wins_over_flatpak(self):
        self.desktop('org.x.App.desktop', 'Name=User\nExec=gimp\n')
        self.desktop('org.x.App.desktop', 'Name=Flatpak\nExec=flatpak run org.x.App\n', 'flatpak/exports/share/applications')
        lines, _, _ = brook_apps.build_index(profile_bin=self.bin)
        self.assertEqual([l.split('\t')[0] for l in lines], ['User'])


class WebApp(unittest.TestCase):
    def test_app_id(self):
        self.assertEqual(webapp.chromium_app_id('https://web.whatsapp.com/'), 'chrome-web.whatsapp.com__-Default')
        self.assertEqual(webapp.chromium_app_id('https://x.example/chat/room?a=1'), 'chrome-x.example__chat_room-Default')

    def test_head_parser(self):
        h = webapp.Head()
        h.feed('<html><head><title> Hi </title><link rel="manifest" href="/m.json">'
               '<link rel="apple-touch-icon" sizes="180x180" href="/a.png"><link rel="icon" href="/f.ico"></head>')
        self.assertEqual((h.title.strip(), h.manifest, [i['src'] for i in h.icons]), ('Hi', '/m.json', ['/a.png', '/f.ico']))

    def test_icon_preference(self):
        icons = [{'src': 'a.png', 'sizes': '48x48'}, {'src': 'b.png', 'sizes': '512x512', 'purpose': 'maskable'},
                 {'src': 'c.png', 'sizes': '192x192'}, {'src': 'd.svg'}]
        self.assertEqual([i['src'] for i in sorted(icons, key=webapp.icon_score, reverse=True)],
                         ['d.svg', 'b.png', 'c.png', 'a.png'])


if __name__ == '__main__':
    unittest.main()
