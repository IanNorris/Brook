#!/usr/bin/env python3
"""Fixture checks for the Python-free live Linux collector (host-side only)."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('dell_linux_audio_snapshot.sh')


class ShellAudioReferenceTests(unittest.TestCase):
    def test_missing_devices_complete_without_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            env = dict(os.environ, BROOK_AUDIO_SYSROOT=directory)
            result = subprocess.run(['sh', str(SCRIPT)], env=env, text=True, capture_output=True, check=True)
            self.assertTrue(result.stdout.startswith('BROOK_AUDIO_SHELL_V1\n'))
            self.assertTrue(result.stdout.rstrip().endswith('END_BROOK_AUDIO_SHELL_V1'))
            self.assertIn('(unavailable)', result.stdout)

    def test_matching_devices_and_card_are_captured(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            acpi = root / 'sys/bus/acpi/devices/INT3438:00'
            acpi.mkdir(parents=True)
            (acpi / 'resource').write_text('0x1000 0x1fff\n')
            machine = root / 'sys/bus/platform/devices/broadwell-audio'
            machine.mkdir(parents=True)
            (machine / 'driver').symlink_to('/fake/broadwell-audio')
            codec = root / 'sys/bus/i2c/devices/2-001c'
            codec.mkdir(parents=True)
            (codec / 'driver').symlink_to('/fake/rt286')
            unrelated = root / 'sys/bus/i2c/devices/2-0040'
            unrelated.mkdir()
            (unrelated / 'driver').symlink_to('/fake/unrelated')
            card = root / 'sys/class/sound/card1'
            card.mkdir(parents=True)
            (card / 'id').write_text('broadwellrt286\n')
            (card / 'device').symlink_to(machine)
            topology = root / 'proc/asound/card0/codec#0'
            topology.parent.mkdir(parents=True)
            topology.write_text('Codec: Realtek ALC3263\nVendor Id: 0x10ec0288\n')
            env = dict(os.environ, BROOK_AUDIO_SYSROOT=directory)
            text = subprocess.run(['sh', str(SCRIPT)], env=env, text=True, capture_output=True, check=True).stdout
            for expected in ('DEVICE INT3438:00', '0x1000 0x1fff', 'DEVICE broadwell-audio',
                             'DEVICE 2-001c', 'CARD card1', 'broadwellrt286'):
                self.assertIn(expected, text)
            self.assertIn('Codec: Realtek ALC3263', text)
            self.assertNotIn('DEVICE 2-0040', text)


if __name__ == '__main__':
    unittest.main()
