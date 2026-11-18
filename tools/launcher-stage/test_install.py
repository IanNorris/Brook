"""Host test for install.py: check mode, create-only install, link swaps,
Apps-manifest edits and --rollback, under a private BROOK_STAGE_ROOT."""
import hashlib, io, json, os, subprocess, sys, tarfile, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
STORE = '/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-brook-test-launchers'
DEP = '/nix/store/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-bash'
OLD_IDX = ('Firefox\t/nix/profile/bin/brook-firefox\tfirefox.rgba\tNetwork;\tbrook-hardware-firefox\n'
           'VSCodium\tcodium --new-window\tvscodium.rgba\tDevelopment;\tcodium\n')


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='brook-stage-')
        self.root = Path(self.tmp.name)/'root'
        self.bundle = Path(self.tmp.name)/'bundle'
        self.bundle.mkdir()
        for d in ('nix/store', 'nix/profile/bin', 'nix/share/applications/icons'):
            (self.root/d).mkdir(parents=True)
        (self.root/DEP.lstrip('/')).mkdir()
        (self.root/'nix/profile/bin/brook-quake2').symlink_to('/nix/store/old/bin/brook-quake2')
        (self.root/'nix/share/applications/applications.idx').write_text(OLD_IDX)
        files = {'bin/brook-quake2': (b'#!/bin/sh\n', True), 'bin/brook-codium': (b'#!/bin/sh\n', True),
                 'share/applications/brook-quake2.desktop': (b'[Desktop Entry]\n', False),
                 'share/brook-icons/brook-quake2.rgba': (b'\x01' * 2304, False)}
        entry = {'files': {}, 'links': {}, 'dirs': ['bin', 'share', 'share/applications', 'share/brook-icons'], 'exec': []}
        with tarfile.open(self.bundle/'store.tar', 'w') as t:
            for d in [''] + entry['dirs']:
                info = tarfile.TarInfo((STORE + ('/' + d if d else '')).lstrip('/')); info.type = tarfile.DIRTYPE; info.mode = 0o755
                t.addfile(info)
            for name, (data, exe) in files.items():
                info = tarfile.TarInfo((STORE + '/' + name).lstrip('/')); info.size = len(data); info.mode = 0o755 if exe else 0o644
                t.addfile(info, io.BytesIO(data))
                entry['files'][name] = hashlib.sha256(data).hexdigest()
                if exe: entry['exec'].append(name)
        (self.bundle/'manifest.json').write_text(json.dumps({STORE: entry}))
        (self.bundle/'closure.txt').write_text(STORE + '\n' + DEP + '\n')
        (self.bundle/'links.json').write_text(json.dumps({
            '/nix/profile/bin/brook-quake2': STORE + '/bin/brook-quake2',
            '/nix/profile/bin/brook-codium': STORE + '/bin/brook-codium',
            '/nix/profile/share/applications/brook-quake2.desktop': STORE + '/share/applications/brook-quake2.desktop'}))
        (self.bundle/'apps.json').write_text(json.dumps({
            'icons': {'brook-quake2.rgba': STORE + '/share/brook-icons/brook-quake2.rgba'},
            'upsert': [{'app_id': 'brook-quake2', 'name': 'Quake II', 'exec': '--uid 1000 /nix/profile/bin/brook-quake2',
                        'icon': 'brook-quake2.rgba', 'categories': 'Game;'}],
            'rewrite_exec': [{'program': 'codium', 'exec': '--uid 1000 /nix/profile/bin/brook-codium'}]}))

    def tearDown(self): self.tmp.cleanup()

    def run_install(self, *extra):
        b = self.bundle
        args = [sys.executable, str(HERE/'install.py')]
        if extra[:1] != ('--rollback',):
            args += [str(b/'store.tar'), str(b/'manifest.json'), str(b/'closure.txt'), str(b/'links.json'), 'TX1']
        r = subprocess.run(args + list(extra), env=dict(os.environ, BROOK_STAGE_ROOT=str(self.root)),
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def test_check_install_rollback(self):
        idx = self.root/'nix/share/applications/applications.idx'
        out = self.run_install('--check', '--apps', str(self.bundle/'apps.json'))
        self.assertIn('CHECK_OK', out)
        self.assertIn('APPS_REWRITE VSCodium: --uid 1000 /nix/profile/bin/brook-codium --new-window', out)
        self.assertIn('APPS_ADD Quake II', out)
        self.assertEqual(idx.read_text(), OLD_IDX)
        self.assertFalse((self.root/STORE.lstrip('/')).exists())

        out = self.run_install('--apps', str(self.bundle/'apps.json'))
        self.assertIn('LAUNCHER_STAGE_OK', out)
        self.assertEqual(idx.read_text(), OLD_IDX.replace('codium --new-window', '--uid 1000 /nix/profile/bin/brook-codium --new-window') +
                         'Quake II\t--uid 1000 /nix/profile/bin/brook-quake2\tbrook-quake2.rgba\tGame;\tbrook-quake2\n')
        self.assertEqual(os.readlink(self.root/'nix/profile/share/applications/brook-quake2.desktop'),
                         STORE + '/share/applications/brook-quake2.desktop')
        self.assertEqual((self.root/'nix/share/applications/icons/brook-quake2.rgba').read_bytes(), b'\x01' * 2304)
        # A second run is refused (create-only); check mode reports why.
        self.assertIn('CHECK_BLOCKED', self.run_install('--check', '--apps', str(self.bundle/'apps.json')))

        out = self.run_install('--rollback', 'TX1')
        self.assertIn('ROLLBACK_OK', out)
        self.assertEqual(idx.read_text(), OLD_IDX)
        self.assertEqual(os.readlink(self.root/'nix/profile/bin/brook-quake2'), '/nix/store/old/bin/brook-quake2')
        self.assertFalse(os.path.lexists(self.root/'nix/profile/bin/brook-codium'))
        self.assertFalse(os.path.lexists(self.root/'nix/profile/share/applications/brook-quake2.desktop'))
        self.assertFalse((self.root/'nix/share/applications/icons/brook-quake2.rgba').exists())

    def test_missing_closure_blocks(self):
        os.rmdir(self.root/DEP.lstrip('/'))
        out = self.run_install('--check')
        self.assertIn('MISSING ' + DEP, out)
        self.assertIn('CHECK_BLOCKED', out)


if __name__ == '__main__':
    unittest.main()
