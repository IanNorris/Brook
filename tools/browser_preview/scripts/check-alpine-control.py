"""Native TCG control of the unmodified Alpine ISO; visual acceptance is separate."""
from pathlib import Path
import subprocess,socket,json,time,shutil
r=Path(__file__).resolve().parents[1];out=r/'artifacts'/('alpine-native-'+time.strftime('%Y%m%dT%H%M%SZ',time.gmtime()));out.mkdir()
args=[shutil.which('qemu-system-x86_64'),'-machine','pc','-cpu','max','-accel','tcg,thread=single,tb-size=64','-m','384M','-smp','1','-vga','std','-display','none','-nic','none','-serial','file:'+str(out/'serial.log'),'-monitor','none','-qmp','unix:'+str(out/'qmp.sock')+',server=on,wait=off','-no-reboot','-no-shutdown','-bios',str(r/'dist/images/OVMF_202302_ROM.fd'),'-drive','if=ide,media=cdrom,format=raw,readonly=on,file='+str(r/'dist/images/controls/alpine-virt-3.24.1-x86_64.iso'),'-boot','order=d,menu=on']
(out/'argv.json').write_text(json.dumps(args,indent=2))
with (out/'qemu.log').open('w') as log:
 p=subprocess.Popen(args,stdout=log,stderr=log);s=None
 try:
  end=time.monotonic()+15
  while not (out/'qmp.sock').exists() and p.poll() is None and time.monotonic()<end:time.sleep(.1)
  s=socket.socket(socket.AF_UNIX);s.settimeout(5);s.connect(str(out/'qmp.sock'));f=s.makefile('rwb',buffering=0);json.loads(f.readline())
  def command(name,arguments=None):
   f.write((json.dumps({'execute':name,'arguments':arguments or {}})+'\n').encode())
   while True:
    value=json.loads(f.readline())
    if 'return' in value or 'error' in value:return value
  command('qmp_capabilities');time.sleep(55)
  shot=command('screendump',{'filename':str(out/'screen.ppm')})
  from PIL import Image
  Image.open(out/'screen.ppm').save(out/'screen.png')
  (out/'result.json').write_text(json.dumps({'guest':'Unmodified Alpine ISO','firmware':'same combined OVMF ROM as browser','visualAcceptance':'pending','screen':shot},indent=2))
  print(out,flush=True)
 finally:
  if p.poll() is None:
   try:command('quit')
   except Exception:p.terminate()
  code=p.wait(timeout=10)
  (out/'exit.json').write_text(json.dumps({'exit':code}))
  if s:s.close()
