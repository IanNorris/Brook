#!/usr/bin/env python3
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import prepare_hardware_kernel_pair as pair
from test_hardware_recovery_fsck import elf


@unittest.skipUnless(all(shutil.which(x) for x in ('mkfs.fat', 'mmd', 'mcopy', 'mtype', 'mdir', 'mattrib')),
                     'requires FAT tools')
class PreparePair(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.image = self.root / 'source.img'
        with self.image.open('wb') as f:
            f.truncate(8 * 1024 * 1024)
        subprocess.run(['mkfs.fat', str(self.image)], check=True, stdout=subprocess.DEVNULL)
        for name in ('DRIVERS', 'ETC', 'BOOT'):
            subprocess.run(['mmd', '-i', str(self.image), '::/' + name], check=True)
        self.build = self.root / 'build'
        (self.build / 'drivers').mkdir(parents=True)
        (self.build / 'BROOK.elf').write_bytes(b'kernel')
        for name in ('xhci.mod', 'sched_eevdf.mod'):
            (self.build / 'drivers' / name).write_bytes(('new-' + name).encode())
            old = self.root / name
            old.write_bytes(('old-' + name).encode())
            subprocess.run(['mcopy', '-i', str(self.image), str(old), '::/DRIVERS/' + name], check=True)
        self.policy = self.root / 'policy'
        self.policy.write_bytes(b'preserve boot policy\n')
        subprocess.run(['mcopy', '-i', str(self.image), str(self.policy), '::/ETC/policy'], check=True)

    def prepare(self, out, payload='test-pair', version=64):
        with patch.object(pair, 'module_version', return_value=version):
            return pair.prepare(self.image, self.build, 64, payload, out, pair.digest(b'kernel'))

    def test_replaces_all_modules_and_preserves_policy(self):
        out = self.root / 'out'
        result = self.prepare(out)
        self.assertEqual(len(result['modules']), 2)
        self.assertEqual(subprocess.check_output(['mtype', '-i', str(out / 'INITRD.IMG'), '::/ETC/policy']),
                         self.policy.read_bytes())
        self.assertEqual(json.loads((out / 'manifest.json').read_text()), result)
        self.assertEqual(subprocess.check_output(['mtype', '-i', str(self.image), '::/DRIVERS/xhci.mod']),
                         b'old-xhci.mod')

    def test_wrong_abi_and_missing_module_do_not_publish(self):
        out = self.root / 'out'
        with self.assertRaises(ValueError):
            self.prepare(out, version=63)
        self.assertFalse(out.exists())
        (self.build / 'drivers/xhci.mod').unlink()
        with self.assertRaises(FileNotFoundError):
            self.prepare(out)
        self.assertFalse(out.exists())

    def test_hidden_module_is_also_replaced(self):
        subprocess.run(['mattrib', '-i', str(self.image), '+h', '::/DRIVERS/xhci.mod'], check=True)
        result = self.prepare(self.root / 'out')
        self.assertEqual(len(result['modules']), 2)

    def test_existing_output_and_invalid_payload_are_rejected(self):
        out = self.root / 'out'
        with self.assertRaises(ValueError):
            self.prepare(out, payload='../escape')
        out.mkdir()
        with self.assertRaises(FileExistsError):
            self.prepare(out)
        self.assertEqual(list(out.iterdir()), [])

    def test_unexpected_kernel_is_rejected(self):
        out = self.root / 'out'
        with self.assertRaises(ValueError):
            pair.prepare(self.image, self.build, 64, 'test', out, '0' * 64)
        self.assertFalse(out.exists())

    def test_optional_panic_exporter_is_copied_and_recorded(self):
        exporter=self.root/'exporter';exporter.write_bytes(elf())
        out=self.root/'out'
        with patch.object(pair, 'module_version', return_value=64):
            result=pair.prepare(self.image,self.build,64,'test',out,pair.digest(b'kernel'),exporter)
        self.assertEqual(result['panic_journal_exporter']['sha256'],pair.digest(exporter.read_bytes()))
        self.assertEqual(subprocess.check_output(['mtype','-i',str(out/'INITRD.IMG'),'::/BOOT/PANICLOG.ELF']),exporter.read_bytes())
        exporter.write_bytes(b'invalid')
        with patch.object(pair, 'module_version', return_value=64):
            with self.assertRaises(ValueError):
                pair.prepare(self.image,self.build,64,'test',self.root/'bad',pair.digest(b'kernel'),exporter)
        self.assertFalse((self.root/'bad').exists())

    def test_optional_fat_checker_requires_pin_and_is_read_back(self):
        checker=self.root/'fat-checker';checker.write_bytes(elf())
        with patch.object(pair,'module_version',return_value=64), patch.object(pair,'FAT_CHECKER_SHA256',pair.digest(checker.read_bytes())):
            result=pair.prepare(self.image,self.build,64,'test',self.root/'with-fat',pair.digest(b'kernel'),fat_checker=checker)
        self.assertEqual(result['fat_checker']['sha256'],pair.digest(checker.read_bytes()))
        self.assertEqual(subprocess.check_output(['mtype','-i',str(self.root/'with-fat/INITRD.IMG'),'::/BOOT/FATFSCK.ELF']),checker.read_bytes())
        with patch.object(pair,'module_version',return_value=64):
            with self.assertRaisesRegex(ValueError,'does not match'):
                pair.prepare(self.image,self.build,64,'test',self.root/'wrong-fat',pair.digest(b'kernel'),fat_checker=checker)
        self.assertFalse((self.root/'wrong-fat').exists())

    @unittest.skipUnless(shutil.which('cc'), 'requires a C compiler')
    def test_real_elf_module_metadata(self):
        source = self.root / 'module.c'
        obj = self.root / 'module.o'
        source.write_text('__attribute__((section(".modinfo"))) const unsigned int info[] = {0x4b524f42, 64};\n')
        subprocess.run(['cc', '-c', str(source), '-o', str(obj)], check=True)
        self.assertEqual(pair.module_version(obj.read_bytes()), 64)
        source.write_text('__attribute__((section(".modinfo"))) const unsigned int info[] = {0, 64};\n')
        subprocess.run(['cc', '-c', str(source), '-o', str(obj)], check=True)
        with self.assertRaises(ValueError):
            pair.module_version(obj.read_bytes())


if __name__ == '__main__':
    unittest.main()
