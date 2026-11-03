#!/usr/bin/env python3
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from prepare_ssd_recovery_pair import INIT, prepare
from test_hardware_recovery_fsck import elf


def run(*args):
    return subprocess.run(args, check=True, stdout=subprocess.DEVNULL)


class RecoveryPairTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        self.kernel = self.source / 'kernel.elf'
        self.kernel.write_bytes(elf())
        self.image = self.source / 'INITRD.IMG'
        with self.image.open('wb') as stream:
            stream.truncate(4 * 1024 * 1024)
        run('mformat', '-i', str(self.image), '::')
        for directory in ('BOOT', 'ETC'):
            run('mmd', '-i', str(self.image), '::' + directory)
        item = self.root / 'item'
        item.write_bytes(b'remove or replace')
        for dest in ('BOOT/WALLPAPER.RAW', 'BOOT/INIT.RC',
                     'ETC/boot-app-snapshot', 'ETC/writable-desktop'):
            run('mcopy', '-i', str(self.image), str(item), '::' + dest)
        item.write_bytes(b'1\n')
        for dest in ('ETC/ssd-block-tools', 'ETC/ssd-install-mode', 'ETC/ahci-write-target'):
            run('mcopy', '-i', str(self.image), str(item), '::' + dest)
        self.checker = self.root / 'checker'
        self.checker.write_bytes(elf())
        self.manifest()

    def tearDown(self):
        self.temp.cleanup()

    def manifest(self):
        files = {}
        for p in (self.kernel, self.image):
            data = p.read_bytes()
            files[p.name] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        (self.source / 'manifest.json').write_text(json.dumps({'payload_id': 'test', 'files': files}))

    def test_prepares_checker_and_boot_record_without_touching_source(self):
        original = hashlib.sha256(self.image.read_bytes()).digest()
        output = self.root / 'out'
        result = prepare(self.source, self.checker, Path('/bin/true'), output)
        self.assertIn('not installed', result['scope'])
        self.assertEqual(hashlib.sha256(self.image.read_bytes()).digest(), original)
        self.assertEqual(subprocess.check_output(['mtype', '-i', str(output / 'INITRD.IMG'),
                                                  '::BOOT/INIT.RC']), INIT)
        self.assertEqual(subprocess.check_output(['mtype', '-i', str(output / 'INITRD.IMG'),
                                                  '::BOOT/E2FSCK.ELF']), self.checker.read_bytes())
        for name in ('boot-app-snapshot', 'writable-desktop'):
            self.assertNotEqual(subprocess.run(['mtype', '-i', str(output / 'INITRD.IMG'),
                                                '::ETC/' + name], stdout=subprocess.DEVNULL,
                                               stderr=subprocess.DEVNULL).returncode, 0)
        selection = (output / 'SELECT.BIN').read_bytes()
        self.assertEqual(selection[:8], b'BROOKUP1')
        self.assertEqual(struct.unpack_from('<Q', selection, 24)[0], len(self.kernel.read_bytes()))
        self.assertEqual(selection[40:72], hashlib.sha256(self.kernel.read_bytes()).digest())
        self.assertEqual(selection[72:104], hashlib.sha256((output / 'INITRD.IMG').read_bytes()).digest())

    def test_bad_source_or_checker_never_produces_a_pair(self):
        output = self.root / 'out'
        self.kernel.write_bytes(b'changed')
        with self.assertRaises(ValueError):
            prepare(self.source, self.checker, Path('/bin/true'), output)
        self.assertFalse(output.exists())
        self.kernel.write_bytes(elf())
        self.checker.write_bytes(b'not an ELF')
        with self.assertRaisesRegex(ValueError, 'checker size'):
            prepare(self.source, self.checker, Path('/bin/true'), output)
        self.assertFalse(output.exists())

    def test_rejects_wrong_validator_and_conflicting_selector(self):
        output = self.root / 'out'
        with self.assertRaises(subprocess.CalledProcessError):
            prepare(self.source, self.checker, Path('/bin/false'), output)
        self.assertFalse(output.exists())
        selector = self.root / 'selector'
        selector.write_bytes(b'unexpected')
        run('mcopy', '-i', str(self.image), str(selector), '::ETC/ahci-data-volume')
        self.manifest()
        with self.assertRaisesRegex(ValueError, 'conflicting boot mount selector'):
            prepare(self.source, self.checker, Path('/bin/true'), output)
        self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
