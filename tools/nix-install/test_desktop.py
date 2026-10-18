"""Exercise the installer's real desktop collector with private filesystem paths."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent


class DesktopTests(unittest.TestCase):
    def test_vlc_wrapper_requires_real_app_and_executable_wrapper(self):
        with tempfile.TemporaryDirectory(prefix='brook-desk-') as directory:
            root = Path(directory)
            paths = dict(PROFILE_DIR='profile', PROFILE_BIN='profile/bin',
                         PROFILE_APPS='profile/share/applications',
                         MANIFEST_PATH='profile/manifest.tsv', BROOK_BIN='bin',
                         APPS_DIR='apps', APPS_IDX='apps/applications.idx',
                         APPS_ICONS_DIR='apps/icons')
            for name in ('profile/bin', 'profile/share/applications', 'bin'):
                (root/name).mkdir(parents=True, exist_ok=True)
            source = root/'driver.c'
            source.write_text('#define main installer_main\n#include "nix-install.c"\n'
                              '#undef main\nint main(void) { refresh_desktop_entries(); return 0; }\n')
            definitions = [f'-D{name}="{root/path}"' for name, path in paths.items()]
            binary = root/'collector'
            subprocess.run([os.environ.get('HOST_CC', 'cc'), '-O2', *definitions,
                            '-I', str(ROOT), str(source), '-o', str(binary)], check=True)
            app = root/'profile/bin/vlc'
            app.write_text('#!/bin/sh\n'); app.chmod(0o755)
            (root/'profile/share/applications/vlc.desktop').write_text(
                '[Desktop Entry]\nType=Application\nName=VLC\nExec=vlc --started-from-file %U\n')
            def collect():
                subprocess.run([str(binary)], check=True, capture_output=True)
                return (root/'apps/applications.idx').read_text()
            self.assertIn('VLC\tvlc --started-from-file\t', collect())
            wrapper = root/'bin/brook-vlc'
            wrapper.write_text('#!/bin/sh\n'); wrapper.chmod(0o644)
            self.assertIn('VLC\tvlc --started-from-file\t', collect())
            wrapper.chmod(0o755)
            self.assertIn(f'VLC\t--uid 1000 {wrapper} --started-from-file\t', collect())
            for command in (str(app), f'"{app}"'):
                (root/'profile/share/applications/vlc.desktop').write_text(
                    f'[Desktop Entry]\nType=Application\nName=VLC\nExec={command} --started-from-file %U\n')
                self.assertIn(f'VLC\t--uid 1000 {wrapper} --started-from-file\t', collect())
            app.unlink()
            self.assertEqual(collect(), '')
            # The same absolute-path correction applies to browser/editor
            # entries while preserving Chromium's existing identity policy.
            for program in ('chromium', 'firefox', 'codium', 'vscodium'):
                app = root/'profile/bin'/program
                app.write_text('#!/bin/sh\n'); app.chmod(0o755)
                (root/'profile/share/applications'/f'{program}.desktop').write_text(
                    f'[Desktop Entry]\nName={program}\nExec="{app}" --test-argument %U\n')
                wrapper = root/'bin'/('brook-codium' if program=='vscodium' else f'brook-{program}')
                wrapper.write_text('#!/bin/sh\n'); wrapper.chmod(0o755)
                identity = '' if program=='chromium' else '--uid 1000 '
                self.assertIn(f'{program}\t{identity}{wrapper} --test-argument\t', collect())


if __name__ == '__main__':
    unittest.main()
