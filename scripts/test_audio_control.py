#!/usr/bin/env python3
"""QEMU master-volume acceptance; run inside nix-shell after building kernel.

Creates disposable files only. Captures through QEMU's WAV output backend;
never emits speaker audio or accesses host USB devices. Exercises the real
user syscall and /dev/dsp paths plus an absent-device boot.
"""
import os,subprocess,shutil,argparse,json,socket,time,wave,array,collections,tempfile
from pathlib import Path
from audio_control_wave import volume_phase_runs, has_ordered_phases, steady_tone_frames
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',type=Path,required=True,help='New directory for disposable test images and captures')
parser.add_argument('--accelerator',choices=('kvm','tcg'),default='kvm')
parser.add_argument('--controller',choices=('ich9-intel-hda','intel-hda'),default='ich9-intel-hda')
parser.add_argument('--tone-only',action='store_true',help='Verify a steady 441 Hz tone instead of the volume matrix')
args=parser.parse_args()
base=args.output.resolve();base.mkdir(parents=True,exist_ok=False)
out=base/'probe'
accelerator=args.accelerator
controller=args.controller
tone_only=args.tone_only
cpu='host' if accelerator=='kvm' else 'max,+rdrand'
def run(*args):subprocess.run(list(map(str,args)),check=True)
run(os.environ['MUSL_CC'],'-static','-Wall','-Wextra','-Werror','-O2',root/'src/tests/test_audio_control_guest/main.c','-o',base/'AUDIOCTL.ELF')
run('python3',root/'scripts/make_uefi_probe.py','--output',out,'--driver','intel_hda','--display-width','800','--display-height','600')
media=str(out/'probe.raw')+'@@1048576';initrd=out/'test-initrd.img';shutil.copy2(out/'payload/INITRD.IMG',initrd)
run('mcopy','-i',initrd,base/'AUDIOCTL.ELF','::BOOT/AUDIOCTL.ELF')
fixture_args=' tone-only' if tone_only else ''
if accelerator=='tcg':fixture_args+=' legacy-drain'
rc=out/'TEST.RC';rc.write_text('set wm\nset vfb none\nkconsole\nvolume\nrun /boot/AUDIOCTL.ELF'+fixture_args+'\n')
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
 def close(self):
  self.f.close();self.s.close()
for mode in ['hda','no-device']:
 d=out/mode;d.mkdir();vars=d/'VARS.fd';shutil.copy2(os.environ['OVMF_VARS'],vars);vars.chmod(0o600)
 qmp_dir=tempfile.TemporaryDirectory(prefix='brook-qmp-');qmp_path=Path(qmp_dir.name)/'qmp.sock'
 disk=out/'probe.raw'
 if mode=='no-device':
  disk=d/'probe.raw';subprocess.run(['cp','--reflink=auto',str(out/'probe.raw'),str(disk)],check=True)
  initrd=d/'INITRD.IMG';shutil.copy2(out/'test-initrd.img',initrd)
  rc=d/'TEST.RC';rc.write_text('set wm\nset vfb none\nkconsole\nrun /boot/AUDIOCTL.ELF no-device\n')
  subprocess.run(['mcopy','-o','-i',str(initrd),str(rc),'::BOOT/INIT.RC'],check=True)
  subprocess.run(['mcopy','-o','-i',str(disk)+'@@1048576',str(initrd),'::INITRD.IMG'],check=True)
 args=['qemu-system-x86_64','-machine','q35','-accel',accelerator,'-cpu',cpu,'-smp','4','-m','2048','-nodefaults',
 '-drive','if=pflash,format=raw,readonly=on,file='+os.environ['OVMF_CODE'],'-drive','if=pflash,format=raw,file='+str(vars),
 '-device','VGA','-device','qemu-xhci,id=usbc','-drive','if=none,id=stick,format=raw,readonly=on,file='+str(disk),
 '-device','usb-storage,bus=usbc.0,drive=stick,bootindex=1','-display','none','-serial','file:'+str(d/'serial.log'),
 '-qmp','unix:'+str(qmp_path)+',server=on,wait=off','-no-reboot','-nic','none']
 if mode=='hda':args+=['-audiodev','wav,id=sound,path='+str(d/'output.wav')+',out.frequency=44100,out.channels=2,out.format=s16','-device',controller,'-device','hda-output,audiodev=sound,use-timer='+('on' if accelerator=='kvm' else 'off')]
 (d/'argv.json').write_text(json.dumps(args,indent=2));log=(d/'qemu.log').open('x');owner=subprocess.Popen(args,stdout=log,stderr=subprocess.STDOUT);q=None
 try:
  deadline=time.monotonic()+120
  while time.monotonic()<deadline:
   assert owner.poll() is None,'QEMU ended early'
   if not q and qmp_path.exists():q=Qmp(qmp_path)
   serial=(d/'serial.log').read_text(errors='replace') if (d/'serial.log').exists() else ''
   assert 'AUDIO_CONTROL_FAIL' not in serial,serial[-1500:]
   assert 'PANIC_HEX:' not in serial
   if ('AUDIO_CONTROL_OK' if mode=='hda' else 'AUDIO_CONTROL_NO_DEVICE_OK') in serial:break
   time.sleep(.1)
  else:raise TimeoutError(serial[-1500:])
  q.call('quit');assert owner.wait(timeout=20)==0
  result=dict(passed=True,mode=mode,accelerator=accelerator,tone_only=tone_only)
  if mode=='hda':
   expected_id='293e' if controller=='ich9-intel-hda' else '2668'
   assert 'intel_hda: QEMU HDA 8086:'+expected_id in serial
   result.update(controller=controller,pci_id='8086:'+expected_id)
   with wave.open(str(d/'output.wav')) as w:
    assert w.getsampwidth()==2 and w.getnchannels()==2 and w.getframerate()==44100
    samples=array.array('h',w.readframes(w.getnframes()))
   counts=collections.Counter(abs(x) for x in samples)
   if tone_only:
    steady=steady_tone_frames(samples)
    assert steady>=22050,('no 0.5-second steady 441 Hz stereo run',steady)
    result.update(sample_rate=44100,tone_hz=441,steady_frames=steady,total_frames=len(samples)//2,
                  out_of_range_samples=sum(n for level,n in counts.items() if level>8000),
                  max_amplitude=max(counts))
   else:
    for level in [0,2000,4000,8000]:assert counts[level]>50000,(level,counts.most_common(8))
    assert max(counts)<=8000,max(counts)
    runs=volume_phase_runs(samples)
    assert has_ordered_phases(runs),('unexpected volume phase order',runs)
    result['amplitude_sample_counts']={str(n):counts[n] for n in [0,2000,4000,8000]}
    result['max_amplitude']=max(counts)
    result['sustained_amplitude_runs']=runs
  (d/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('VOLUME_QEMU_PASS',mode,flush=True)
 finally:
  if owner.poll() is None:
   if q:
    try:q.call('quit');owner.wait(timeout=20)
    except BaseException:owner.terminate();owner.wait(timeout=20)
   else:owner.terminate();owner.wait(timeout=20)
  if q:q.close()
  log.close()
  qmp_dir.cleanup()
