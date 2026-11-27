#!/usr/bin/env python3
"""Host test: brook-steam exports the set's NVK ICD marker as VK_DRIVER_FILES.

    python3 scripts/test_steam_vk_icd.py
"""
import importlib.util
from pathlib import Path
import tempfile
import unittest

LAUNCHER = Path(__file__).resolve().parent / 'workloads/brook_steam_launcher.py'
spec = importlib.util.spec_from_file_location('brook_steam_launcher', LAUNCHER)
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


class VulkanIcdEnvironment(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.marker = Path(self.tmp.name) / 'steam-vk-icd'

    def tearDown(self):
        self.tmp.cleanup()

    def existing_store_json(self):
        for icd in Path('/nix/store').glob('*-mesa-*/share/vulkan/icd.d/*.json'):
            return str(icd)
        self.skipTest('no Mesa ICD JSON in /nix/store')

    def test_marker_sets_driver_files(self):
        icd = self.existing_store_json()
        self.marker.write_text(icd + '\n')
        env = {}
        self.assertEqual(launcher.vulkan_icd_environment(env, self.marker), icd)
        self.assertEqual(env['VK_DRIVER_FILES'], icd)

    def test_caller_choice_wins(self):
        self.marker.write_text(self.existing_store_json() + '\n')
        for key in ('VK_DRIVER_FILES', 'VK_ICD_FILENAMES'):
            env = {key: '/x.json'}
            self.assertIsNone(launcher.vulkan_icd_environment(env, self.marker))
            self.assertEqual(env, {key: '/x.json'})

    def test_missing_or_bad_marker_is_ignored(self):
        env = {}
        self.assertIsNone(launcher.vulkan_icd_environment(env, self.marker))
        for text in ('relative/icd.json\n', '/nix/store/none-mesa/share/vulkan/icd.d/x.json\n',
                     '/etc/passwd\n', ''):
            self.marker.write_text(text)
            self.assertIsNone(launcher.vulkan_icd_environment(env, self.marker))
        self.assertEqual(env, {})


if __name__ == '__main__':
    unittest.main()
