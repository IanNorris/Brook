import argparse, os, pathlib, shutil, subprocess, sys, time
root=pathlib.Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description='Stage and verify recovery on a disposable QEMU SSD fixture')
parser.add_argument('--fixture',type=pathlib.Path,required=True)
parser.add_argument('--initrd',type=pathlib.Path,required=True)
parser.add_argument('--helper',type=pathlib.Path,required=True)
parser.add_argument('--kernel',type=pathlib.Path,required=True)
parser.add_argument('--serial',default='BROOK-SLOT-QA')
parser.add_argument('--output',type=pathlib.Path,required=True)
parser.add_argument('--alternate',action='store_true',help='Verify separate RECOV2 staging preserves existing recovery')
parser.add_argument('--replacement',action='store_true',help='Verify RECOVNEW staging preserves both recovery pairs')
a=parser.parse_args()
if a.alternate and a.replacement:parser.error('choose one staging directory')
if a.fixture.is_symlink() or not a.fixture.is_file():raise ValueError('fixture must be a regular image file')
sys.path.insert(0,str(root/'tools'))
from hardware_update_protocol import record
out=a.output.absolute()
out.mkdir(exist_ok=False)
subprocess.run(['cp','--reflink=auto',str(a.fixture),str(out/'disk.raw')],check=True)
shutil.copyfile(a.initrd,out/'initrd.img')
(out/'INIT.RC').write_text('set wm\nset vfb none\nsched eevdf\nkconsole\nrun --console --uid 0 /boot/RECSTAGE.ELF /media/system/KERNEL/BROOK.ELF /media/system/INITRD.IMG /media/system/TESTSEL.BIN\n')
for source,target in ((a.helper,'::BOOT/RECSTAGE.ELF'),(out/'INIT.RC','::BOOT/INIT.RC')):
 subprocess.run(['mcopy','-o','-i',str(out/'initrd.img'),str(source),target],check=True)
media=str(out/'disk.raw')+'@@1048576'
legacy={}
if a.alternate or a.replacement:
 for directory in (('RECOVERY','RECOV2') if a.replacement else ('RECOVERY',)):
  for name in ('KERNEL.ELF','INITRD.IMG','SELECT.BIN'):
   legacy[directory+'/'+name]=subprocess.check_output(['mtype','-i',media,'::'+directory+'/'+name])
 loader=subprocess.check_output(['mtype','-i',media,'::EFI/BOOT/BOOTX64.EFI'])
kern=a.kernel
(out/'TESTSEL.BIN').write_bytes(record(1,0,kern.read_bytes(),(out/'initrd.img').read_bytes()))
for source,target in ((out/'initrd.img','::INITRD.IMG'),(kern,'::KERNEL/BROOK.ELF'),(out/'TESTSEL.BIN','::TESTSEL.BIN')):
 subprocess.run(['mcopy','-o','-i',media,str(source),target],check=True)
shutil.copyfile(os.environ['OVMF_VARS'],out/'vars.fd')
argv=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','4','-m','4096','-nodefaults','-drive','if=pflash,format=raw,readonly=on,file='+os.environ['OVMF_CODE'],'-drive','if=pflash,format=raw,file='+str(out/'vars.fd'),'-device','VGA,vgamem_mb=64','-drive','if=none,id=sata,format=raw,file='+str(out/'disk.raw'),'-device','ide-hd,drive=sata,bus=ide.0,bootindex=1,serial='+a.serial,'-display','none','-serial','file:'+str(out/'serial.log'),'-monitor','none','-no-reboot']
with (out/'qemu.log').open('wb') as log:
 p=subprocess.Popen(argv,stdout=log,stderr=subprocess.STDOUT)
 try:
  for _ in range(120):
   time.sleep(.5)
   if (out/'serial.log').exists() and 'SSD_RECOVERY_STAGE_' in (out/'serial.log').read_text(errors='replace'):break
   if p.poll() is not None:break
 finally:
  if p.poll() is None:p.terminate()
  p.wait(timeout=10)
serial=(out/'serial.log').read_text(errors='replace') if (out/'serial.log').exists() else ''
print('\n'.join(x for x in serial.splitlines() if 'SSD_RECOVERY' in x or 'ahci system:' in x or 'RECSTAGE' in x))
if 'SSD_RECOVERY_STAGE_OK' not in serial:raise SystemExit('guest staging did not pass')
for name,expected in (('KERNEL.ELF',kern.read_bytes()),('INITRD.IMG',(out/'initrd.img').read_bytes()),('SELECT.BIN',(out/'TESTSEL.BIN').read_bytes())):
 actual=subprocess.check_output(['mtype','-i',media,('::RECOVNEW/' if a.replacement else '::RECOV2/' if a.alternate else '::RECOVERY/')+name])
 if actual!=expected:raise SystemExit(name+' mismatch')
if a.alternate or a.replacement:
 for name,expected in legacy.items():
  if subprocess.check_output(['mtype','-i',media,'::'+name])!=expected:raise SystemExit('recovery changed: '+name)
 if subprocess.check_output(['mtype','-i',media,'::EFI/BOOT/BOOTX64.EFI'])!=loader:raise SystemExit('default loader changed')
print('SSD_RECOVERY_STAGE_READBACK_PASS')
