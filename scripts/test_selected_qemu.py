#!/usr/bin/env python3
"""Check selected-profile QEMU inputs before an offline image is assembled."""

import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import archive_public_qemu
import make_public_qemu
from selected_launchers import collect_selected


class SelectedLaunchersTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.nix = Path(temp.name) / 'nix'
        self.profile = self.nix / 'profile'
        self.store = self.nix / 'store'
        self.apps = self.profile / 'share/applications'
        self.output = self.nix / 'share/applications'
        self.store.mkdir(parents=True)
        self.apps.mkdir(parents=True)
        (self.profile / 'bin').mkdir()
        for executable in ('calculator', 'cli-extra'):
            path = self.profile / 'bin' / executable
            path.write_text('')
            path.chmod(0o755)
        # An extra package can have a desktop entry with the selected title.
        # It must not shadow the selected app even when it sorts earlier.
        for name, executable in [('a-cli-extra', 'cli-extra'), ('wayland-calc', 'calculator')]:
            (self.apps / f'{name}.desktop').write_text(
                '[Desktop Entry]\nType=Application\nName=Calculator\n'
                f'Exec=/nix/profile/bin/{executable}\nComment=Add numbers\n'
            )
        icon = patch('collect_desktop_entries.convert_icon_to_rgba', return_value=True)
        icon.start()
        self.addCleanup(icon.stop)

    def test_subset_preserves_desktop_uid_and_comment(self):
        collect_selected(self.store, self.output, self.profile, ['wayland-calc'], hardware=True)
        self.assertEqual(
            (self.output / 'applications.idx').read_text(),
            'Calculator\t--uid 1000 /nix/profile/bin/calculator\t\t\twayland-calc\tAdd numbers\n',
        )

    def test_missing_or_ineligible_launcher_is_not_silently_omitted(self):
        with self.assertRaisesRegex(ValueError, 'does not exist: missing'):
            collect_selected(self.store, self.output, self.profile, ['missing'])
        (self.profile / 'bin/calculator').chmod(0o644)
        with self.assertRaisesRegex(ValueError, 'No eligible desktop-user command'):
            collect_selected(self.store, self.output, self.profile, ['wayland-calc'])


class SelectedQemuTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.profile = '/nix/store/' + '0' * 32 + '-brook-profile'
        self.selection = {
            'schema': 'brook-profile-selection/1',
            'profile': self.profile,
            'packages': [],
            'intelVulkanDriver': None,
            'manifest': {'requiredCapabilities': {}, 'redistributable': False, 'launcherIds': []},
        }

    def test_selected_store_must_match_digest_and_realized_profile(self):
        image = self.root / 'STORE.IMG'
        image.write_bytes(b'image')
        metadata = {'profile': self.profile, 'profile_selection': self.selection,
                    'sha256': hashlib.sha256(b'image').hexdigest(), 'bytes': 5}
        self.assertEqual(make_public_qemu.developer_seed(self.root, metadata, self.root), image)
        image.write_bytes(b'other')
        with self.assertRaisesRegex(ValueError, 'does not match its seed manifest'):
            make_public_qemu.developer_seed(self.root, metadata, self.root)
        metadata['profile'] += '-other'
        with self.assertRaisesRegex(ValueError, 'does not match the requested store root'):
            make_public_qemu.developer_seed(self.root, metadata, self.root)

    def test_public_mode_rejects_selected_seed_and_archive(self):
        (self.root / 'manifest.json').write_text(
            '{"public_inputs_only":true,"profile_selection":{}}'
        )
        output = self.root / 'output'
        with patch('sys.argv', ['make_public_qemu.py', '--seed', str(self.root), '--output', str(output)]):
            with self.assertRaises(SystemExit):
                make_public_qemu.main()
        self.assertFalse(output.exists())
        (self.root / 'manifest.json').write_text('{"status":"private-selected-profile"}')
        with patch('sys.argv', ['archive_public_qemu.py', '--bundle', str(self.root), '--output', str(output)]):
            with self.assertRaises(SystemExit):
                archive_public_qemu.main()
        self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
