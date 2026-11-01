#!/usr/bin/env python3
"""Host-side fixtures for the read-only Linux PCI resource collector."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('dell_linux_pci_resources.sh')


class PciResourceCollectorTests(unittest.TestCase):
    def run_collector(self, root):
        env = dict(os.environ, BROOK_PCI_SYSROOT=str(root))
        return subprocess.run(['sh', str(SCRIPT)], env=env, text=True,
                              capture_output=True, check=True).stdout

    def test_missing_inventory_is_explicit(self):
        with tempfile.TemporaryDirectory() as root:
            output = self.run_collector(root)
            self.assertIn('(no PCI devices available)', output)
            self.assertTrue(output.rstrip().endswith('END_BROOK_PCI_RESOURCES_V1'))

    def test_resources_and_missing_fields_are_reported(self):
        with tempfile.TemporaryDirectory() as root:
            device = Path(root) / 'sys/bus/pci/devices/0000:00:1b.0'
            device.mkdir(parents=True)
            (device / 'vendor').write_text('0x8086\n')
            (device / 'device').write_text('0x9ca0\n')
            (device / 'resource').write_text('0xf7418000 0xf741bfff 0x200\n')
            output = self.run_collector(root)
            self.assertIn('DEVICE 0000:00:1b.0', output)
            self.assertIn('class=\n(unavailable)', output)
            self.assertIn('0xf7418000 0xf741bfff 0x200', output)


if __name__ == '__main__':
    unittest.main()
