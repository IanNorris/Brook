#!/usr/bin/env python3
"""Control boot of the browser image; uses disposable QEMU snapshot state."""
import gzip,json,os,pathlib,socket,subprocess,time,shutil
root=pathlib.Path(__file__).resolve().parents[1]
profile=os.environ.get('NATIVE_FIRMWARE_PROFILE','pflash')
if profile not in ('pflash','rom'):raise ValueError('Unknown native firmware profile')
out=root/'artifacts'/('native-rom-control-'+time.strftime('%Y-%m-%dT%H-%M-%SZ',time.gmtime()) if profile=='rom' else 'native-image-control');out.mkdir(exist_ok=True)
with gzip.open(root/'dist/images/brook-boot.img.gz','rb') as source,(out/'boot.img').open('wb') as target:shutil.copyfileobj(source,target)
qmp=out/'qmp.sock';qmp.unlink(missing_ok=True)
args=[shutil.which('qemu-system-x86_64'),'-machine','pc','-cpu','max','-accel','tcg,thread=single','-m','384M','-smp','1','-vga','std','-display','none','-nic','none','-serial',f'file:{out}/serial.log','-monitor','none','-qmp',f'unix:{qmp},server=on,wait=off','-no-reboot','-no-shutdown','-snapshot','-drive',f'if=pflash,format=raw,unit=0,readonly=on,file={root}/dist/images/OVMF_202302_CODE.fd','-drive',f'if=pflash,format=raw,unit=1,file={root}/dist/images/OVMF_202302_VARS.fd','-drive',f'if=virtio,format=raw,file={out}/boot.img']
if profile=='rom':
 begin=args.index('-drive');last=begin+4
 args[begin:last]=['-bios',str(root/'dist/images/OVMF_202302_ROM.fd')]
with (out/'qemu.log').open('w') as log:
 proc=subprocess.Popen(args,stdout=log,stderr=log)
 try:
  end=time.monotonic()+20
  while not qmp.exists() and proc.poll() is None and time.monotonic()<end:time.sleep(.1)
  sock=socket.socket(socket.AF_UNIX);sock.settimeout(5);sock.connect(str(qmp));stream=sock.makefile('rwb',buffering=0)
  greeting=json.loads(stream.readline())
  def command(name,arguments=None):
   req={'execute':name}
   if arguments is not None:req['arguments']=arguments
   stream.write(json.dumps(req).encode()+b'\n')
   while True:
    value=json.loads(stream.readline())
    if 'return' in value or 'error' in value:return value
  command('qmp_capabilities')
  while time.monotonic()<end:
   serial=(out/'serial.log').read_text(errors='replace') if (out/'serial.log').exists() else ''
   if 'COMPOSITOR: thread started' in serial and 'WM: created window' in serial:break
   if proc.poll() is not None:break
   time.sleep(.25)
  regs=command('human-monitor-command',{'command-line':'info registers'})
  screen=command('screendump',{'filename':str(out/'screen.ppm')})
  result={'profile':profile,'artifactDir':str(out),'booted':'COMPOSITOR: thread started' in serial,'desktop':'WM: created window' in serial,'serialTail':serial[-5000:],'registers':regs,'screen':screen,'qemu':greeting}
  if (out/'screen.ppm').exists():
   from PIL import Image
   image=Image.open(out/'screen.ppm');image.save(out/'screen.png')
   result['nonBlackPixels']=sum(1 for pixel in image.convert('RGB').resize((32,24)).getdata() if any(pixel))
   result['png']=str(out/'screen.png')
  (out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result))
  command('quit');sock.close()
 finally:
  try:proc.wait(timeout=5)
  except subprocess.TimeoutExpired:proc.terminate();proc.wait(timeout=5)
  print(json.dumps({'trackedQemuStopped':proc.poll() is not None,'exitCode':proc.returncode,'profile':profile}))
if not result.get('desktop') or not result.get('nonBlackPixels',0):raise SystemExit(1)
