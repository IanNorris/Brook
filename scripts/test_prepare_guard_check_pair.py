import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import prepare_guard_check_pair as target
from test_hardware_recovery_fsck import elf


class GuardPairTest(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name);self.build=self.root/'build';self.build.mkdir()
        self.kernel=elf();(self.build/'BROOK.elf').write_bytes(self.kernel)
        (self.build/'CMakeCache.txt').write_text(''.join(f'{k}:BOOL={v}\n' for k,v in target.REQUIRED.items()))
        self.commands=[{'file':'/source/'+name,'command':'clang++ -DBROOK_SSD_GUARD_CHECK=1 -c '+name} for name in
                       ('kernel.cpp','ahci_readonly.cpp','ahci_system_volume.cpp','guard_checker_runner.cpp')]
        (self.build/'compile_commands.json').write_text(json.dumps(self.commands))
        self.checker=self.root/'checker';self.checker.write_bytes(elf())
        self.controller=self.root/'controller';self.disk=self.root/'disk';self.esp=self.root/'esp';self.data=self.root/'data'
        self.controller.write_text(json.dumps(dict(valid=True,controller_count=1,vendor=32902,device=10530,bus=0,slot=31,function=2,ports=[dict(index=0,signature=257)])))
        self.disk.write_text(json.dumps(dict(ready=True,quarantined=False,port=0,sectors=262144,logical_sector_bytes=512,physical_sector_bytes=512,model='QEMU HARDDISK',serial='BROOK-RECOVERY-QA')))
        self.esp.write_bytes(b'BRKESP1\n'+b'01'*16+b'\n'+b'02'*16+b'\n2048,131072\n')
        self.data.write_bytes(b'BRKDATA1\n'+b'01'*16+b'\n'+b'03'*16+b'\n133120,126976\n'+b'04'*16+b'\n')
        self.output=self.root/'out'

    def tearDown(self):self.tmp.cleanup()

    def prepare(self,profile='qemu-fixture'):
        with mock.patch.object(target,'CHECKER_SHA',hashlib.sha256(self.checker.read_bytes()).hexdigest()):
            return target.prepare(self.build,hashlib.sha256(self.kernel).hexdigest(),self.checker,Path('/bin/true'),
                                  self.controller,self.disk,self.esp,self.data,profile,self.output)

    def test_creates_minimal_verified_pair(self):
        result=self.prepare();image=self.output/'INITRD.IMG'
        self.assertIn('not installed',result['scope'])
        self.assertEqual(result['guard_identity'],'01'*16+'03'*16+'04'*16)
        for name,expected in [('ETC/ahci-boot-volume',self.esp.read_bytes()),('ETC/ahci-data-volume',self.data.read_bytes()),
                              ('BOOT/FATFSCK.ELF',self.checker.read_bytes()),('ETC/ahci-observe',b'1\n')]:
            self.assertEqual(subprocess.check_output(['mtype','-i',str(image),'::'+name]),expected)
        for name in ('ETC/ssd-install-mode','ETC/guard-pending-test','ETC/boot-app-image','BOOT/E2FSCK.ELF'):
            self.assertNotEqual(subprocess.run(['mtype','-i',str(image),'::'+name],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode,0)
        for name in ('DEV/DRI','SYS/DEV/CHAR','PROC'):
            subprocess.run(['mdir','-i',str(image),'::'+name],check=True,stdout=subprocess.DEVNULL)

    def test_rejects_mixed_or_overlapping_identity(self):
        self.data.write_bytes(self.data.read_bytes().replace(b'01'*16,b'05'*16,1))
        with self.assertRaisesRegex(ValueError,'inconsistent'):self.prepare()
        self.assertFalse(self.output.exists())

    def test_rejects_fixture_as_physical(self):
        with self.assertRaisesRegex(ValueError,'fixture identity'):self.prepare('physical')
        self.assertFalse(self.output.exists())

    def test_rejects_unsafe_kernel_flags(self):
        self.commands[0]['command']+=' -DBROOK_SSD_GUARD_EXT2_TEST=1'
        (self.build/'compile_commands.json').write_text(json.dumps(self.commands))
        with self.assertRaisesRegex(ValueError,'unsafe compile flags'):self.prepare()
        self.assertFalse(self.output.exists())

    def test_rejects_split_define_and_guard_undef(self):
        for extra in ('-D BROOK_SSD_INSTALLER=1','-UBROOK_SSD_GUARD_CHECK','-U BROOK_SSD_GUARD_CHECK','@flags.rsp'):
            with self.subTest(extra=extra):
                self.commands[0]['command']='clang++ -DBROOK_SSD_GUARD_CHECK=1 '+extra
                (self.build/'compile_commands.json').write_text(json.dumps(self.commands))
                with self.assertRaises(ValueError):self.prepare()
                self.assertFalse(self.output.exists())

    def test_rejects_checker_hash_mismatch(self):
        with self.assertRaisesRegex(ValueError,'checker does not match'):
            target.prepare(self.build,hashlib.sha256(self.kernel).hexdigest(),self.checker,Path('/bin/true'),
                           self.controller,self.disk,self.esp,self.data,'qemu-fixture',self.output)
        self.assertFalse(self.output.exists())

    def test_automatic_pair_requires_matching_build_and_both_pinned_checkers(self):
        required=dict(target.REQUIRED);required['BROOK_SSD_AUTO_RECOVERY']='ON'
        (self.build/'CMakeCache.txt').write_text(''.join(f'{k}:BOOL={v}\n' for k,v in required.items()))
        flags=' -DBROOK_SSD_AUTO_RECOVERY=1 -DBROOK_SSD_GUARD_TRANSACTION=1 -DBROOK_SSD_GUARD_REPAIR=1 -DBROOK_SSD_GUARD_COMPLETION=1'
        for command in self.commands:command['command']+=flags
        (self.build/'compile_commands.json').write_text(json.dumps(self.commands))
        digest=hashlib.sha256(self.checker.read_bytes()).hexdigest()
        with mock.patch.object(target,'CHECKER_SHA',digest),mock.patch.object(target,'E2FSCK_SHA',digest):
            result=target.prepare(self.build,hashlib.sha256(self.kernel).hexdigest(),self.checker,Path('/bin/true'),
                self.controller,self.disk,self.esp,self.data,'qemu-fixture',self.output,True,self.checker)
        self.assertEqual(result['ext2_checker_sha256'],digest)
        self.assertIn('automatic',result['scope'])
        self.assertEqual(subprocess.check_output(['mtype','-i',str(self.output/'INITRD.IMG'),'::BOOT/E2FSCK.ELF']),self.checker.read_bytes())
        self.output.rename(self.root/'completed')
        (self.build/'drivers').mkdir();(self.build/'drivers/virtio_net.mod').write_bytes(b'test-module')
        module_hash=hashlib.sha256(b'test-module').hexdigest()
        with mock.patch.object(target,'CHECKER_SHA',digest),mock.patch.object(target,'E2FSCK_SHA',digest), \
             mock.patch('prepare_hardware_kernel_pair.module_version',return_value=65):
            network=target.prepare(self.build,hashlib.sha256(self.kernel).hexdigest(),self.checker,Path('/bin/true'),
                self.controller,self.disk,self.esp,self.data,'qemu-fixture',self.output,True,self.checker,True,65,module_hash)
        self.assertEqual(network['recovery_network_module']['name'],'virtio_net.mod')
        self.assertEqual(subprocess.check_output(['mtype','-i',str(self.output/'INITRD.IMG'),'::DRIVERS/virtio_net.mod']),b'test-module')
        self.output.rename(self.root/'completed-network')
        with mock.patch.object(target,'CHECKER_SHA',digest),mock.patch.object(target,'E2FSCK_SHA',digest):
            with self.assertRaisesRegex(ValueError,'hash mismatch'):
                target.prepare(self.build,hashlib.sha256(self.kernel).hexdigest(),self.checker,Path('/bin/true'),
                    self.controller,self.disk,self.esp,self.data,'qemu-fixture',self.output,True,self.checker,True,65,'0'*64)
        with mock.patch.object(target,'CHECKER_SHA',digest),mock.patch.object(target,'E2FSCK_SHA',digest), \
             mock.patch('prepare_hardware_kernel_pair.module_version',return_value=64):
            with self.assertRaisesRegex(ValueError,'ABI mismatch'):
                target.prepare(self.build,hashlib.sha256(self.kernel).hexdigest(),self.checker,Path('/bin/true'),
                    self.controller,self.disk,self.esp,self.data,'qemu-fixture',self.output,True,self.checker,True,65,module_hash)
        self.assertFalse(self.output.exists())
        self.commands[0]['command']+=' -DBROOK_SSD_GUARD_PENDING_TEST=1'
        (self.build/'compile_commands.json').write_text(json.dumps(self.commands))
        with self.assertRaisesRegex(ValueError,'unsafe compile flags'):
            target.prepare(self.build,hashlib.sha256(self.kernel).hexdigest(),self.checker,Path('/bin/true'),
                self.controller,self.disk,self.esp,self.data,'qemu-fixture',self.output,True,self.checker)
        self.assertFalse(self.output.exists())


if __name__=='__main__':unittest.main()
