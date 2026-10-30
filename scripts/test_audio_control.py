#!/usr/bin/env python3
"""QEMU master-volume acceptance; run inside nix-shell after building kernel.

Creates disposable files only. Captures through QEMU's WAV output backend;
never emits speaker audio or accesses host USB devices. Exercises the real
user syscall and /dev/dsp paths plus an absent-device boot.
"""
import os,subprocess,shutil,argparse,json,socket,time,wave,array,collections
from pathlib import Path
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',type=Path,required=True,help='New directory for disposable test images and captures')
args=parser.parse_args()
base=args.output.resolve();base.mkdir(parents=True,exist_ok=False)
out=base/'probe'
def run(*args):subprocess.run(list(map(str,args)),check=True)
run(os.environ['MUSL_CC'],'-static','-Wall','-Wextra','-Werror','-O2',root/'src/tests/test_audio_control_guest/main.c','-o',base/'AUDIOCTL.ELF')
run('python3',root/'scripts/make_uefi_probe.py','--output',out,'--driver','intel_hda','--display-width','800','--display-height','600')
media=str(out/'probe.raw')+'@@1048576';initrd=out/'test-initrd.img';shutil.copy2(out/'payload/INITRD.IMG',initrd)
run('mcopy','-i',initrd,base/'AUDIOCTL.ELF','::BOOT/AUDIOCTL.ELF')
rc=out/'TEST.RC';rc.write_text('set wm\nset vfb none\nkconsole\nvolume\nrun /boot/AUDIOCTL.ELF\n')
run('mcopy','-o','-i',initrd,rc,'::BOOT/INIT.RC');run('mcopy','-o','-i',media,initrd,'::INITRD.IMG')
print('VOLUME_TEST_PREPARED',flush=True)

class Qmp:
 def __init__(self,path):
  self.s=socket.socket(socket.AF_UNIX);self.s.settimeout(10);self.s.connect(str(path));self.f=self.s.makefile('rwb',buffering=0);self.f.readline();self.call('qmp_capabilities')
 def call(self,cmd,args=None):
  self.f.write((json.dumps(dict(execute=cmd,**({'arguments':args} if args else {})))+'\n').encode())
  while True:
   r=json.loads(self.f.readline())
   if 'return' in r:return r['return']
   if 'error' in r:raise RuntimeError(r)
for mode in ['hda','no-device']:
 d=out/mode;d.mkdir();vars=d/'VARS.fd';shutil.copy2(os.environ['OVMF_VARS'],vars);vars.chmod(0o600)
 disk=out/'probe.raw'
 if mode=='no-device':
  disk=d/'probe.raw';subprocess.run(['cp','--reflink=auto',str(out/'probe.raw'),str(disk)],check=True)
  initrd=d/'INITRD.IMG';shutil.copy2(out/'test-initrd.img',initrd)
  rc=d/'TEST.RC';rc.write_text('set wm\nset vfb none\nkconsole\nrun /boot/AUDIOCTL.ELF no-device\n')
  subprocess.run(['mcopy','-o','-i',str(initrd),str(rc),'::BOOT/INIT.RC'],check=True)
  subprocess.run(['mcopy','-o','-i',str(disk)+'@@1048576',str(initrd),'::INITRD.IMG'],check=True)
 args=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','4','-m','2048','-nodefaults',
 '-drive','if=pflash,format=raw,readonly=on,file='+os.environ['OVMF_CODE'],'-drive','if=pflash,format=raw,file='+str(vars),
 '-device','VGA','-device','qemu-xhci,id=usbc','-drive','if=none,id=stick,format=raw,readonly=on,file='+str(disk),
 '-device','usb-storage,bus=usbc.0,drive=stick,bootindex=1','-display','none','-serial','file:'+str(d/'serial.log'),
 '-qmp','unix:'+str(d/'qmp.sock')+',server=on,wait=off','-no-reboot','-nic','none']
 if mode=='hda':args+=['-audiodev','wav,id=sound,path='+str(d/'output.wav')+',out.frequency=44100,out.channels=2,out.format=s16','-device','ich9-intel-hda','-device','hda-output,audiodev=sound']
 (d/'argv.json').write_text(json.dumps(args,indent=2));log=(d/'qemu.log').open('x');owner=subprocess.Popen(args,stdout=log,stderr=subprocess.STDOUT);q=None
 try:
  deadline=time.monotonic()+120
  while time.monotonic()<deadline:
   assert owner.poll() is None,'QEMU ended early'
   if not q and (d/'qmp.sock').exists():q=Qmp(d/'qmp.sock')
   serial=(d/'serial.log').read_text(errors='replace') if (d/'serial.log').exists() else ''
   assert 'AUDIO_CONTROL_FAIL' not in serial,serial[-1500:]
   assert 'PANIC_HEX:' not in serial
   if ('AUDIO_CONTROL_OK' if mode=='hda' else 'AUDIO_CONTROL_NO_DEVICE_OK') in serial:break
   time.sleep(.1)
  else:raise TimeoutError(serial[-1500:])
  q.call('quit');assert owner.wait(timeout=20)==0
  result=dict(passed=True,mode=mode)
  if mode=='hda':
   with wave.open(str(d/'output.wav')) as w:
    assert w.getsampwidth()==2 and w.getnchannels()==2 and w.getframerate()==44100
    samples=array.array('h',w.readframes(w.getnframes()))
   counts=collections.Counter(abs(x) for x in samples)
   for level in [0,2000,4000,8000]:assert counts[level]>50000,(level,counts.most_common(8))
   assert max(counts)<=8000,max(counts)
   result['amplitude_sample_counts']={str(n):counts[n] for n in [0,2000,4000,8000]}
   result['max_amplitude']=max(counts)
  (d/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('VOLUME_QEMU_PASS',mode,flush=True)
 finally:
  if owner.poll() is None:
   if q:
    try:q.call('quit');owner.wait(timeout=20)
    except BaseException:owner.terminate();owner.wait(timeout=20)
   else:owner.terminate();owner.wait(timeout=20)
  log.close()
