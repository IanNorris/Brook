#!/usr/bin/env python3
"""Check selected-profile QEMU inputs before an offline image is assembled."""

import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import archive_public_qemu
import make_public_qemu
import image_profile
import prepare_hardware_store
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
            'manifest': {'target': 'qemu', 'requiredCapabilities': {},
                         'redistributable': False, 'launcherIds': []},
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

    def test_embedded_manifest_binds_selection_and_package_metadata(self):
        embedded = self.root / 'share/brook/profile.json'
        embedded.parent.mkdir(parents=True)
        selection = self.selection
        manifest = selection['manifest']
        manifest['packages'] = []
        with patch('image_profile.store_path', return_value=self.root):
            with self.assertRaisesRegex(ValueError, 'no readable embedded manifest'):
                image_profile.verify_realized_selection(selection)
            embedded.write_text(json.dumps(manifest))
            self.assertEqual(image_profile.verify_realized_selection(selection), self.root)
            manifest['preset'] = 'network'
            with self.assertRaisesRegex(ValueError, 'original profile expression'):
                image_profile.verify_realized_selection(selection)
            del manifest['preset']
            manifest['redistributable'] = True
            with self.assertRaisesRegex(ValueError, 'differs from its embedded manifest'):
                image_profile.verify_realized_selection(selection)
            embedded.write_text(json.dumps(manifest))
            selection['packages'] = [{'name': 'extra', 'version': '1', 'store': '0' * 32 + '-extra'}]
            with self.assertRaisesRegex(ValueError, 'package metadata differs'):
                image_profile.verify_realized_selection(selection)

    def test_measured_store_free_space_changes_only_automatic_capacity(self):
        mib = prepare_hardware_store.MIB
        image = self.root / 'STORE.IMG'
        image.touch()
        with patch('prepare_hardware_store.free_space', return_value=160 * mib):
            with self.assertRaisesRegex(ValueError, 'at least 2016 MiB'):
                prepare_hardware_store.grow_selected_store(image, 1664 * mib, 4096 * mib, False)
            with self.assertRaisesRegex(ValueError, 'at least 2016 MiB'):
                prepare_hardware_store.grow_selected_store(image, 1664 * mib, 2000 * mib, True)
        self.assertEqual(image.stat().st_size, 0)
        with patch('prepare_hardware_store.free_space', side_effect=[160 * mib, 512 * mib]):
            with patch('prepare_hardware_store.run') as resize:
                result = prepare_hardware_store.grow_selected_store(image, 1664 * mib, 4096 * mib, True)
            resize.assert_called_once_with('resize2fs', image)
        self.assertEqual(result, 2024 * mib)
        self.assertEqual(image.stat().st_size, result)

    def test_usb_requires_usb_profile_boot_capability_and_xhci(self):
        image = self.root / 'STORE.IMG'
        image.write_bytes(b'image')
        metadata = {'profile': self.profile, 'profile_selection': self.selection,
                    'sha256': hashlib.sha256(b'image').hexdigest(), 'bytes': 5}
        with self.assertRaisesRegex(ValueError, 'not composed for the usb target'):
            make_public_qemu.developer_seed(self.root, metadata, self.root, usb=True)
        self.selection['manifest']['target'] = 'usb'
        with patch('kernel_capabilities.read_kernel_capabilities', return_value={'capabilities': {}}):
            with self.assertRaisesRegex(ValueError, 'usb-partition-desktop'):
                make_public_qemu.developer_seed(self.root, metadata, self.root, usb=True)
        capabilities = {'capabilities': {'usb-partition-desktop': 1}}
        with patch('kernel_capabilities.read_kernel_capabilities', return_value=capabilities) as read:
            with self.assertRaisesRegex(ValueError, 'requires the xHCI driver module'):
                make_public_qemu.developer_seed(self.root, metadata, self.root, usb=True)
            driver = self.root / 'kernel/drivers/xhci.mod'
            driver.parent.mkdir(parents=True)
            driver.touch()
            self.assertEqual(make_public_qemu.developer_seed(self.root, metadata, self.root, usb=True), image)
            read.assert_called_with(self.root / 'kernel/BROOK.elf')

    def test_public_mode_rejects_selected_seed_and_archive(self):
        (self.root / 'manifest.json').write_text(json.dumps({
            'public_inputs_only': True,
            'profile_expression': str(make_public_qemu.ROOT / 'nix/release-packages.nix'),
            'profile_selection': self.selection,
        }))
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
