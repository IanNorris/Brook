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
                       ('kernel.cpp','ahci_readonly.cpp','ahci_system_volume.cpp','guard_checker_runner.cpp','shutdown.cpp','intel_bdw_ioctl.cpp')]
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
    def check(self, allow_gpu_submission_timing=False, allow_vulkan_desktop=False, allow_vulkan_partial=False, allow_gpu_blocked_recovery_test=False, expected_gpu_provider=None, allow_gpu_source_trace=False, allow_vulkan_async_partial=False, allow_dell_bcs_present=False, allow_irq_signal_probe=False):
        with mock.patch.object(verifier,'CHECKER_SHA',hashlib.sha256(self.fat).hexdigest()), \
             mock.patch.object(verifier,'E2FSCK_SHA',hashlib.sha256(self.ext).hexdigest()), \
             mock.patch.object(verifier,'module_version',return_value=65):
            return verifier.verify(self.candidate,self.source,self.ref,self.build,verifier.sha(self.kernel),
                                   verifier.sha(self.source.read_bytes()),verifier.sha(self.ref.read_bytes()),65,
                                   allow_gpu_submission_timing=allow_gpu_submission_timing,allow_vulkan_desktop=allow_vulkan_desktop,
                                   expected_vulkan_supervisor=verifier.sha(b'\x7fELF-sup'),expected_vulkan_renderer=verifier.sha(b'\x7fELF-render'),
                                   allow_vulkan_partial=allow_vulkan_partial,allow_gpu_blocked_recovery_test=allow_gpu_blocked_recovery_test,
                                   expected_gpu_provider=expected_gpu_provider,allow_gpu_source_trace=allow_gpu_source_trace,
                                   allow_vulkan_async_partial=allow_vulkan_async_partial,allow_dell_bcs_present=allow_dell_bcs_present,
                                   allow_irq_signal_probe=allow_irq_signal_probe)
    def test_irq_signal_probe_requires_exact_opt_in(self):
        with self.assertRaisesRegex(ValueError, 'Missing or invalid opt-in irq-signal-probe'):
            self.check(allow_irq_signal_probe=True)
        self.copy('ETC/irq-signal-probe', b'bad\n')
        with self.assertRaisesRegex(ValueError, 'Missing or invalid opt-in irq-signal-probe'):
            self.check(allow_irq_signal_probe=True)
        self.copy('ETC/irq-signal-probe', b'1\n')
        with self.assertRaisesRegex(ValueError, 'Unexpected opt-in irq-signal-probe'):
            self.check()
        self.assertEqual(self.check(allow_irq_signal_probe=True)['module_abi'], 65)
    def test_valid_pair(self):
        self.assertEqual(self.check()['modules']['xhci.mod'],verifier.sha(self.module))
    def test_rejects_changed_guard_and_script(self):
        self.copy('ETC/ahci-data-volume',b'bad\n')
        with self.assertRaisesRegex(ValueError,'guard input mismatch'):self.check()
        shutil.copyfile(self.source,self.image);self.copy('BOOT/INIT.RC',b'changed\n')
        with self.assertRaisesRegex(ValueError,'startup script mismatch'):self.check()
    def test_vulkan_start_requires_exact_opt_in(self):
        self.copy('BOOT/INIT.RC',verifier.add_vulkan_desktop_start(b'set wm\n'))
        self.copy('BOOT/VKSUP.ELF',b'\x7fELF-sup');self.copy('BOOT/VKRENDER.ELF',b'\x7fELF-render')
        with self.assertRaisesRegex(ValueError,'startup script mismatch'):self.check()
        self.assertTrue(self.check(allow_vulkan_desktop=True)['vulkan_desktop_enabled'])
        self.copy('BOOT/VKRENDER.ELF',b'\x7fELF-wrong')
        with self.assertRaisesRegex(ValueError,'Vulkan executable mismatch'):self.check(allow_vulkan_desktop=True)
        self.copy('BOOT/VKRENDER.ELF',b'\x7fELF-render')
        self.copy('BOOT/INIT.RC',verifier.add_vulkan_desktop_start(b'set wm\n')+b'run unexpected\n')
        with self.assertRaisesRegex(ValueError,'startup script mismatch'):self.check(allow_vulkan_desktop=True)
    def test_partial_requires_separate_exact_opt_in(self):
        self.copy('BOOT/INIT.RC',verifier.add_vulkan_desktop_start(b'set wm\n',True))
        self.copy('BOOT/VKSUP.ELF',b'\x7fELF-sup');self.copy('BOOT/VKRENDER.ELF',b'\x7fELF-render')
        with self.assertRaisesRegex(ValueError,'startup script mismatch'):self.check(allow_vulkan_desktop=True)
        with self.assertRaisesRegex(ValueError,'requires Vulkan'):self.check(allow_vulkan_partial=True)
        self.assertTrue(self.check(allow_vulkan_desktop=True,allow_vulkan_partial=True)['vulkan_partial_enabled'])
    def test_async_blitter_requires_exact_opt_ins(self):
        self.copy('BOOT/INIT.RC',verifier.add_vulkan_desktop_start(b'set wm\n',async_partial=True))
        self.copy('BOOT/VKSUP.ELF',b'\x7fELF-sup');self.copy('BOOT/VKRENDER.ELF',b'\x7fELF-render')
        self.copy('ETC/dell-gpu-bcs-present',b'1\n')
        with self.assertRaisesRegex(ValueError,'Unexpected opt-in'):
            self.check(allow_vulkan_desktop=True,allow_vulkan_async_partial=True)
        with self.assertRaisesRegex(ValueError,'startup script mismatch'):
            self.check(allow_vulkan_desktop=True,allow_vulkan_partial=True,allow_dell_bcs_present=True)
        self.assertTrue(self.check(allow_vulkan_desktop=True,allow_vulkan_async_partial=True,
                                   allow_dell_bcs_present=True)['vulkan_async_partial_enabled'])
    def test_rejects_incorrect_module(self):
        self.copy('DRIVERS/xhci.mod',b'other')
        with self.assertRaisesRegex(ValueError,'module mismatch'):self.check()
    def test_gpu_source_requires_provider_pin_and_exact_start(self):
        base=b'set wm\nrun --service --once --uid 1000 /boot/INTLWL.ELF\n'
        self.copy('BOOT/INIT.RC',base);shutil.copyfile(self.image,self.source);shutil.copyfile(self.image,self.ref)
        self.copy('BOOT/INIT.RC',verifier.add_vulkan_desktop_start(base,True,True))
        self.copy('BOOT/VKSUP.ELF',b'\x7fELF-sup');self.copy('BOOT/VKRENDER.ELF',b'\x7fELF-render')
        self.copy('BOOT/INTLWL.ELF',b'\x7fELF-provider')
        with self.assertRaisesRegex(ValueError,'startup script mismatch'):
            self.check(allow_vulkan_desktop=True,allow_vulkan_partial=True)
        pin=verifier.sha(b'\x7fELF-provider')
        result=self.check(allow_vulkan_desktop=True,allow_vulkan_partial=True,expected_gpu_provider=pin)
        self.assertEqual(result['vulkan_executables_sha256']['INTLWL.ELF'],pin)
        self.copy('BOOT/INIT.RC',verifier.add_vulkan_desktop_start(base,True,True,True))
        with self.assertRaisesRegex(ValueError,'startup script mismatch'):
            self.check(allow_vulkan_desktop=True,allow_vulkan_partial=True,expected_gpu_provider=pin)
        self.check(allow_vulkan_desktop=True,allow_vulkan_partial=True,expected_gpu_provider=pin,allow_gpu_source_trace=True)
        self.copy('BOOT/INIT.RC',verifier.add_vulkan_desktop_start(base,True,True))
        self.copy('BOOT/INTLWL.ELF',b'\x7fELF-other')
        with self.assertRaisesRegex(ValueError,'Vulkan executable mismatch'):
            self.check(allow_vulkan_desktop=True,allow_vulkan_partial=True,expected_gpu_provider=pin)
    def test_timing_requires_explicit_opt_in(self):
        (self.build/'CMakeCache.txt').write_text(self.cache.replace('BROOK_GPU_SUBMISSION_TIMING:BOOL=OFF','BROOK_GPU_SUBMISSION_TIMING:BOOL=ON'))
        for command in self.commands:command['command']+=' -DBROOK_GPU_SUBMISSION_TIMING=1'
        (self.build/'compile_commands.json').write_text(json.dumps(self.commands))
        with self.assertRaisesRegex(ValueError,'unsafe build option'):self.check()
        self.assertEqual(self.check(allow_gpu_submission_timing=True)['module_abi'],65)
    def test_blocked_recovery_hook_requires_explicit_opt_in(self):
        (self.build/'CMakeCache.txt').write_text(self.cache.replace('BROOK_GPU_BLOCKED_RECOVERY_TEST:BOOL=OFF','BROOK_GPU_BLOCKED_RECOVERY_TEST:BOOL=ON'))
        for command in self.commands:command['command']+=' -DBROOK_GPU_BLOCKED_RECOVERY_TEST=1'
        (self.build/'compile_commands.json').write_text(json.dumps(self.commands))
        with self.assertRaisesRegex(ValueError,'unsafe build option'):self.check()
        self.assertEqual(self.check(allow_gpu_blocked_recovery_test=True)['module_abi'],65)
        self.commands[-1]['command']=self.commands[-1]['command'].replace(' -DBROOK_GPU_BLOCKED_RECOVERY_TEST=1','')
        (self.build/'compile_commands.json').write_text(json.dumps(self.commands))
        with self.assertRaisesRegex(ValueError,'unsafe compile flags'):self.check(allow_gpu_blocked_recovery_test=True)
    def test_rejects_test_hook_and_shutdown_undef(self):
        (self.build/'CMakeCache.txt').write_text(self.cache.replace('BROOK_SSD_GUARDED_DESKTOP_RESET_TEST:BOOL=OFF','BROOK_SSD_GUARDED_DESKTOP_RESET_TEST:BOOL=ON'))
        with self.assertRaisesRegex(ValueError,'unsafe build option'):self.check()
        (self.build/'CMakeCache.txt').write_text(self.cache)
        self.commands[-1]['command']+=' -UBROOK_SSD_GUARDED_DESKTOP_RESET'
        (self.build/'compile_commands.json').write_text(json.dumps(self.commands))
        with self.assertRaisesRegex(ValueError,'unsafe compile flags'):self.check()

if __name__=='__main__':unittest.main()
