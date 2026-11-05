import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

import verify_guarded_desktop_candidate as verifier


@unittest.skipUnless(all(shutil.which(x) for x in ('mformat','mmd','mcopy','mtype','mdir','fsck.fat')),'requires FAT tools')
class VerifyGuardedDesktop(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.root=Path(self.tmp.name)
        self.build=self.root/'build';(self.build/'drivers').mkdir(parents=True)
        self.kernel=b'kernel';(self.build/'BROOK.elf').write_bytes(self.kernel)
        self.cache=''.join(f'{k}:BOOL={v}\n' for k,v in verifier.EXPECTED.items())+'BROOK_SMP_BOOT_TEST_STAGE:STRING=0\n'
        (self.build/'CMakeCache.txt').write_text(self.cache)
        flags={k for k,v in verifier.EXPECTED.items() if v=='ON'}|verifier.DERIVED
        self.commands=[{'file':'/src/'+name,'command':'clang++ -DBROOK_SMP_BOOT_TEST_STAGE=0 '+' '.join('-D'+x+'=1' for x in sorted(flags))} for name in
                       ('kernel.cpp','ahci_readonly.cpp','ahci_system_volume.cpp','guard_checker_runner.cpp','shutdown.cpp')]
        (self.build/'compile_commands.json').write_text(json.dumps(self.commands))
        self.module=b'module';(self.build/'drivers/xhci.mod').write_bytes(self.module)
        self.candidate=self.root/'candidate';self.candidate.mkdir();(self.candidate/'kernel.elf').write_bytes(self.kernel)
        self.image=self.candidate/'INITRD.IMG'
        with self.image.open('wb') as f:f.truncate(8*1024*1024)
        subprocess.run(['mformat','-i',str(self.image),'::'],check=True)
        for name in ('ETC','BOOT','DRIVERS'):subprocess.run(['mmd','-i',str(self.image),'::/'+name],check=True)
        self.fat=b'fat-checker';self.ext=b'ext2-checker'
        values={'ETC/ahci-write-target':b'target\n',
                'ETC/ahci-boot-volume':b'BRKESP1\n'+b'01'*16+b'\n'+b'02'*16+b'\n2048,1000\n',
                'ETC/ahci-data-volume':b'BRKDATA1\n'+b'01'*16+b'\n'+b'03'*16+b'\n4000,2000\n'+b'04'*16+b'\n',
                'BOOT/FATFSCK.ELF':self.fat,'BOOT/E2FSCK.ELF':self.ext,
                'BOOT/INIT.RC':b'set wm\n','DRIVERS/xhci.mod':self.module}
        for name,data in values.items():self.copy(name,data)
        self.source=self.root/'source.img';self.ref=self.root/'reference.img'
        shutil.copyfile(self.image,self.source);shutil.copyfile(self.image,self.ref)
    def copy(self,name,data):
        subprocess.run(['mcopy','-o','-i',str(self.image),'-','::/'+name],input=data,check=True)
    def check(self):
        with mock.patch.object(verifier,'CHECKER_SHA',hashlib.sha256(self.fat).hexdigest()), \
             mock.patch.object(verifier,'E2FSCK_SHA',hashlib.sha256(self.ext).hexdigest()), \
             mock.patch.object(verifier,'module_version',return_value=65):
            return verifier.verify(self.candidate,self.source,self.ref,self.build,verifier.sha(self.kernel),
                                   verifier.sha(self.source.read_bytes()),verifier.sha(self.ref.read_bytes()),65)
    def test_valid_pair(self):
        self.assertEqual(self.check()['modules']['xhci.mod'],verifier.sha(self.module))
    def test_rejects_changed_guard_and_script(self):
        self.copy('ETC/ahci-data-volume',b'bad\n')
        with self.assertRaisesRegex(ValueError,'guard input mismatch'):self.check()
        shutil.copyfile(self.source,self.image);self.copy('BOOT/INIT.RC',b'changed\n')
        with self.assertRaisesRegex(ValueError,'startup script mismatch'):self.check()
    def test_rejects_incorrect_module(self):
        self.copy('DRIVERS/xhci.mod',b'other')
        with self.assertRaisesRegex(ValueError,'module mismatch'):self.check()
    def test_rejects_test_hook_and_shutdown_undef(self):
        (self.build/'CMakeCache.txt').write_text(self.cache.replace('BROOK_SSD_GUARDED_DESKTOP_RESET_TEST:BOOL=OFF','BROOK_SSD_GUARDED_DESKTOP_RESET_TEST:BOOL=ON'))
        with self.assertRaisesRegex(ValueError,'unsafe build option'):self.check()
        (self.build/'CMakeCache.txt').write_text(self.cache)
        self.commands[-1]['command']+=' -UBROOK_SSD_GUARDED_DESKTOP_RESET'
        (self.build/'compile_commands.json').write_text(json.dumps(self.commands))
        with self.assertRaisesRegex(ValueError,'unsafe compile flags'):self.check()

if __name__=='__main__':unittest.main()
