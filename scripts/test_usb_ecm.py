#!/usr/bin/env python3
"""Bounded QEMU USB Ethernet acceptance, with USB storage and HID activity.

Run inside nix-shell after building the kernel. Only disposable images are
opened; no physical USB, host desktop or audio is used. The workload only contacts localhost services;
the guest may also perform its ordinary boot-time DNS probe.
"""
import argparse,hashlib,json,os,shutil,socket,struct,subprocess,threading,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',required=True,type=Path)
args=ap.parse_args();base=args.output.resolve();base.mkdir(parents=True,exist_ok=False);out=base/'probe'
def run(*cmd):subprocess.run(list(map(str,cmd)),check=True,cwd=ROOT)
tcp=socket.socket();tcp.bind(('127.0.0.1',0));port=tcp.getsockname()[1];tcp.listen();tcp.settimeout(90)
udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);udp.bind(('127.0.0.1',port));udp.settimeout(.2)
with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:s.bind(('127.0.0.1',0));udp_forward=s.getsockname()[1]
errors=[];completed=[]
def exact(s,n):
 b=b''
 while len(b)<n:
  chunk=s.recv(n-len(b))
  if not chunk:raise RuntimeError('Incomplete guest upload')
  b+=chunk
 return b
def echo_tcp():
 try:
  s,_=tcp.accept()
  with s:
   s.settimeout(30);b=exact(s,10);assert b==b'ecm-zlp-ok';s.sendall(b)
   for p in range(8):
    b=exact(s,32768);assert b==bytes((i*13+p*7)%251 for i in range(32768));s.sendall(b)
  completed.append('tcp')
 except BaseException as e:errors.append(repr(e))
def echo_udp():
 try:
  expected=bytes((i*13+7*7)%251 for i in range(982));deadline=time.monotonic()+100
  while time.monotonic()<deadline:
   udp.sendto(expected,('127.0.0.1',udp_forward))
   try:b,address=udp.recvfrom(2048)
   except socket.timeout:continue
   assert b==expected;completed.append('udp');return
  raise TimeoutError('UDP echo')
 except BaseException as e:errors.append(repr(e))
threads=[threading.Thread(target=f,daemon=True) for f in [echo_tcp,echo_udp]]
for thread in threads:thread.start()
run(os.environ['MUSL_CC'],'-static','-O2','-Wall','-Wextra','-Werror',ROOT/'src/tests/test_usb_ecm_guest/main.c','-o',base/'ECMTEST.ELF')
run('python3',ROOT/'scripts/make_uefi_probe.py','--output',out,'--driver','xhci','--usb-read-only','--media-mount','/media/usb','--partition-table','mbr','--display-width','800','--display-height','600')
initrd=out/'payload/INITRD.IMG';run('mcopy','-i',initrd,base/'ECMTEST.ELF','::BOOT/ECMTEST.ELF')
rc=base/'INIT.RC';rc.write_text(f'set wm\nset vfb none\nkconsole\nhwprobe on\nrun /boot/ECMTEST.ELF {port}\n')
run('mcopy','-o','-i',initrd,rc,'::BOOT/INIT.RC')
marker=base/'usb-net-probe';marker.write_text('1\n');run('mcopy','-i',initrd,marker,'::ETC/usb-net-probe')
media=str(out/'probe.raw')+'@@1048576';run('mcopy','-o','-i',media,initrd,'::INITRD.IMG')
fixture=base/'READTEST.BIN';size=8*1024*1024;pattern=bytes((i*17+5)%251 for i in range(251))
with fixture.open('xb') as f:
 while size:
  block=(pattern*4096)[:min(size,251*4096)];f.write(block);size-=len(block)
run('mcopy','-i',media,fixture,'::READTEST.BIN')
manifest=json.loads((out/'manifest.json').read_text())
for name,row in manifest['files'].items():
 p=out/'payload'/name;row.update(bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest())
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
variables=base/'VARS.fd';shutil.copy2(os.environ['OVMF_VARS'],variables);variables.chmod(0o600)
with socket.socket() as s:s.bind(('127.0.0.1',0));debug_port=s.getsockname()[1]
qemu=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','4','-m','2048','-nodefaults',
 '-drive','if=pflash,format=raw,readonly=on,file='+os.environ['OVMF_CODE'],'-drive','if=pflash,format=raw,file='+str(variables),
 '-device','VGA','-device','qemu-xhci,id=usbc','-drive','if=none,id=stick,format=raw,readonly=on,file='+str(out/'probe.raw'),
 '-device','usb-storage,bus=usbc.0,port=1,drive=stick,bootindex=1',
 '-device','usb-kbd,bus=usbc.0,port=3','-device','usb-mouse,bus=usbc.0,port=4',
 '-display','none','-serial','file:'+str(base/'serial.log'),'-qmp','unix:'+str(base/'qmp.sock')+',server=on,wait=off','-no-reboot',
 '-netdev',f'user,id=network,hostfwd=tcp:127.0.0.1:{debug_port}-:1234,hostfwd=udp:127.0.0.1:{udp_forward}-:{port},guestfwd=tcp:10.0.2.100:{port}-tcp:127.0.0.1:{port}',
 '-device','usb-net,id=usbnet,bus=usbc.0,port=2,netdev=network',
 '-object','filter-dump,id=wire,netdev=network,file='+str(base/'wire.pcap')]
# Explicit forwards reach private local echo services. slirp must allow UDP
# replies to its host alias, which restrict=on blocks even for host forwarding.
(base/'argv.json').write_text(json.dumps(qemu,indent=2)+'\n')
class Qmp:
 def __init__(self,path):
  self.s=socket.socket(socket.AF_UNIX);self.s.settimeout(10);self.s.connect(str(path));self.f=self.s.makefile('rwb',buffering=0);self.f.readline();self.call('qmp_capabilities')
 def call(self,cmd,args=None):
  self.f.write((json.dumps(dict(execute=cmd,**({'arguments':args} if args else {})))+'\n').encode())
  while True:
   answer=json.loads(self.f.readline())
   if 'return' in answer:return answer['return']
   if 'error' in answer:raise RuntimeError(answer)
log=(base/'qemu.log').open('x');owner=subprocess.Popen(qemu,stdout=log,stderr=subprocess.STDOUT);q=None
try:
 deadline=time.monotonic()+120
 while time.monotonic()<deadline:
  assert owner.poll() is None,'QEMU ended early'
  if not q and (base/'qmp.sock').exists():q=Qmp(base/'qmp.sock')
  serial=(base/'serial.log').read_text(errors='replace') if (base/'serial.log').exists() else ''
  assert 'PANIC_HEX:' not in serial and 'ECM_TEST_FAIL' not in serial,serial[-2500:]
  assert not errors,errors
  if 'ECM_NETWORK_INTEGRITY_OK' in serial:break
  time.sleep(.05)
 else:raise TimeoutError(serial[-3000:])
 with socket.create_connection(('127.0.0.1',debug_port),timeout=8) as s:
  s.settimeout(8);s.sendall(b'@71 mem\n');reply=b''
  while b'BRIDGE_END 71' not in reply and len(reply)<65536:
   chunk=s.recv(8192)
   if not chunk:break
   reply+=chunk
  assert b'BRIDGE_BEGIN 71' in reply and b'BRIDGE_END 71' in reply
  (base/'bridge-reply.bin').write_bytes(reply)
 q.call('device_del',{'id':'usbnet'})
 q.call('human-monitor-command',{'command-line':'sendkey a 100'})
 deadline=time.monotonic()+10
 while time.monotonic()<deadline:
  serial=(base/'serial.log').read_text(errors='replace')
  if 'ECM_USB_STILL_ALIVE' in serial and 'HWKEY: down sc=0x1e' in serial:break
  time.sleep(.05)
 else:raise TimeoutError('USB/HID did not remain responsive after NIC removal')
 q.call('quit');assert owner.wait(timeout=15)==0
 for thread in threads:thread.join(2)
 assert not errors and set(completed)=={'tcp','udp'},(errors,completed)
 sizes=[];raw=(base/'wire.pcap').read_bytes();at=24
 assert raw[:4]==b'\xd4\xc3\xb2\xa1'
 while at+16<=len(raw):
  _,_,captured,_=struct.unpack_from('<4I',raw,at);at+=16
  packet=raw[at:at+captured];at+=captured
  if len(packet)>=34 and packet[12:14]==b'\x08\x00' and packet[26:30]==bytes([10,0,2,15]):sizes.append(len(packet))
 assert 64 in sizes and 1024 in sizes,sorted(set(sizes))
 result=dict(passed=True,tcp_verified_bytes=262144,udp_verified_bytes=982,zero_length_termination_frames=[64,1024],
  idle_rx_seconds=1.5,bridge_framed_reply=True,usb_storage_concurrent=True,usb_storage_after_nic_removal=True,usb_keyboard_after_nic_removal=True)
 (base/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('USB_ECM_ACCEPTANCE_PASS',flush=True)
finally:
 if owner.poll() is None:
  try:
   if q:q.call('quit');owner.wait(timeout=10)
   else:owner.terminate();owner.wait(timeout=10)
  except BaseException:owner.kill();owner.wait(timeout=10)
 tcp.close();udp.close();log.close()
