#!/usr/bin/env python3
"""Bounded QEMU USB Ethernet acceptance, with USB storage and HID activity.

Run inside nix-shell after building the kernel. Only disposable images are
opened; no physical USB, host desktop or audio is used. The workload only contacts localhost services;
the guest may also perform its ordinary boot-time DNS probe.
"""
import argparse,hashlib,json,os,shutil,socket,struct,subprocess,tempfile,threading,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',required=True,type=Path)
ap.add_argument('--rx-depth8',action='store_true',help='Exercise the opt-in eight-request ECM receive path')
ap.add_argument('--tx-depth16',action='store_true',help='Exercise the opt-in sixteen-frame ECM transmit path')
ap.add_argument('--tcp-window16',action='store_true',help='Exercise the opt-in 16 KiB TCP receive-window cap')
ap.add_argument('--tcp-flight2',action='store_true',help='Exercise and capture the experimental two-packet TCP send flight')
ap.add_argument('--tcp-flight4',action='store_true',help='Exercise and capture the experimental four-packet TCP send flight')
ap.add_argument('--tcp-adaptive-flight',action='store_true',help='Exercise the bounded adaptive TCP send flight')
ap.add_argument('--tcp-send-probe',action='store_true',help='Enable and query aggregate TCP sender diagnostics')
ap.add_argument('--network-delay-us',type=int,default=0,help='Optional QEMU filter-buffer interval in microseconds (maximum 50000)')
ap.add_argument('--drop-tcp-packet',action='store_true',help='Drop one guest TCP payload through a private QEMU packet relay')
ap.add_argument('--send-concurrency',action='store_true',help='Also verify two guest writers share a TCP stream without interleaving')
ap.add_argument('--send-close-race',action='store_true',help='Also close a shared descriptor while two guest writes are active')
ap.add_argument('--send-user-copy',action='store_true',help='Also exercise send, writev and multi-iovec sendmsg')
args=ap.parse_args();base=args.output.resolve();base.mkdir(parents=True,exist_ok=False);out=base/'probe'
if args.send_close_race and not args.send_concurrency:ap.error('--send-close-race requires --send-concurrency')
if sum((args.tcp_flight2,args.tcp_flight4,args.tcp_adaptive_flight))>1:ap.error('choose only one flight mode')
if not 0<=args.network_delay_us<=50000:ap.error('network delay must be between 0 and 50000 microseconds')
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
   if args.send_user_copy:
    b=exact(s,1218);assert b==b'X'*1200+b'sendwritevmultiiov';s.sendall(b[1200:])
   if args.send_concurrency:
    b=exact(s,8192);assert b in (b'A'*4096+b'B'*4096,b'B'*4096+b'A'*4096);s.sendall(b)
   if args.send_close_race:
    b=exact(s,1200);assert b==b'C'*1200;s.sendall(b'R')
    b+=exact(s,262144-1200+4096);assert b==b'C'*262144+b'D'*4096
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
run(os.environ['MUSL_CC'],'-static','-pthread','-O2','-Wall','-Wextra','-Werror',ROOT/'src/tests/test_usb_ecm_guest/main.c','-o',base/'ECMTEST.ELF')
run('python3',ROOT/'scripts/make_uefi_probe.py','--output',out,'--driver','xhci','--usb-read-only','--media-mount','/media/usb','--partition-table','mbr','--display-width','800','--display-height','600')
initrd=out/'payload/INITRD.IMG';run('mcopy','-i',initrd,base/'ECMTEST.ELF','::BOOT/ECMTEST.ELF')
rc=base/'INIT.RC';rc.write_text(f'set wm\nset vfb none\nkconsole\nhwprobe on\nrun /boot/ECMTEST.ELF {port}{" concurrency" if args.send_concurrency else ""}{" close-race" if args.send_close_race else ""}{" user-copy" if args.send_user_copy else ""}\n')
run('mcopy','-o','-i',initrd,rc,'::BOOT/INIT.RC')
marker=base/'usb-net-probe';marker.write_text('1\n');run('mcopy','-i',initrd,marker,'::ETC/usb-net-probe')
if args.rx_depth8:run('mcopy','-i',initrd,marker,'::ETC/usb-net-rx8')
if args.tx_depth16:run('mcopy','-i',initrd,marker,'::ETC/usb-net-tx16')
if args.tcp_window16:run('mcopy','-i',initrd,marker,'::ETC/tcp-recv-window-16k')
if args.tcp_flight2:run('mcopy','-i',initrd,marker,'::ETC/tcp-two-packet-flight')
if args.tcp_flight4:run('mcopy','-i',initrd,marker,'::ETC/tcp-four-packet-flight')
if args.tcp_adaptive_flight:run('mcopy','-i',initrd,marker,'::ETC/tcp-adaptive-flight')
if args.tcp_send_probe:run('mcopy','-i',initrd,marker,'::ETC/tcp-send-probe')
if args.send_user_copy:run('mcopy','-i',initrd,marker,'::ETC/tcp-egress-1200')
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
 '-device','usb-net,id=usbnet,bus=usbc.0,port=2,netdev=network']
# Place the capture on the guest side of the delay: backend-to-guest packets
# must be recorded after the buffer releases them, not when the backend emits.
if args.network_delay_us:qemu+=['-object',f'filter-buffer,id=delay,netdev=network,queue=tx,interval={args.network_delay_us}']
loss=None;loss_sockets=None;loss_thread=None;loss_stop=threading.Event();loss_connections=[]
if args.drop_tcp_packet:
 # RX filter traversal is reversed: reinjection must pass the output filter
 # without being redirected a second time. These sockets are private to QEMU.
 loss_sockets=tempfile.TemporaryDirectory(prefix='brook-qemu-loss-')
 loss={'seen':0,'dropped':0,'packet':None,'retransmitted':False,'errors':[]}
 loss_out=Path(loss_sockets.name)/'out';loss_in=Path(loss_sockets.name)/'in'
 qemu+=['-chardev',f'socket,id=lossout,path={loss_out},server=on,wait=off',
        '-chardev',f'socket,id=lossin,path={loss_in},server=on,wait=off',
        '-object','filter-redirector,id=lossin,netdev=network,queue=rx,indev=lossin',
        '-object','filter-redirector,id=lossout,netdev=network,queue=rx,outdev=lossout']
 def relay_loss():
  try:
   with socket.socket(socket.AF_UNIX) as incoming,socket.socket(socket.AF_UNIX) as outgoing:
    loss_connections.extend((incoming,outgoing))
    for sock,path in ((incoming,loss_out),(outgoing,loss_in)):
     deadline=time.monotonic()+15
     while True:
      try:sock.connect(str(path));break
      except (FileNotFoundError,ConnectionRefusedError):
       if time.monotonic()>deadline:raise
       time.sleep(.01)
    incoming.settimeout(120);outgoing.settimeout(120)
    while True:
     header=exact(incoming,4);size=struct.unpack('!I',header)[0]
     if not 14<=size<=65536:raise RuntimeError(f'Invalid packet length {size}')
     packet=exact(incoming,size);drop=False
     if packet[12:14]==b'\x08\x00' and len(packet)>=54 and packet[23]==6:
      ihl=(packet[14]&15)*4;ip_total=struct.unpack_from('!H',packet,16)[0];offset=14+ihl
      fragment=struct.unpack_from('!H',packet,20)[0]
      if (packet[14]>>4)==4 and ihl>=20 and len(packet)>=offset+20 and not(fragment&0x3fff) and 14+ip_total<=len(packet) and struct.unpack_from('!H',packet,offset+2)[0]==port:
       tcp_len=(packet[offset+12]>>4)*4;payload=ip_total-ihl-tcp_len
       seq=struct.unpack_from('!I',packet,offset+4)[0]
       if tcp_len>=20 and payload>=500 and packet[26:30]==bytes([10,0,2,15]):
        flow=packet[26:34].hex()+packet[offset:offset+4].hex()
        item={'flow':flow,'sequence':seq,'bytes':payload,'sha256':hashlib.sha256(packet[offset+tcp_len:14+ip_total]).hexdigest()}
        loss['seen']+=1
        if loss['packet']==item:loss['retransmitted']=True
        if loss['seen']==2:loss['dropped']+=1;loss['packet']=item;drop=True
     if not drop:outgoing.sendall(header+packet)
  except BaseException as error:
   if not loss_stop.is_set():loss['errors'].append(repr(error))
qemu+=['-object','filter-dump,id=wire,netdev=network,file='+str(base/'wire.pcap')]
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
if loss:
 loss_thread=threading.Thread(target=relay_loss,daemon=True);loss_thread.start()
try:
 deadline=time.monotonic()+120
 while time.monotonic()<deadline:
  assert owner.poll() is None,'QEMU ended early'
  if not q and (base/'qmp.sock').exists():q=Qmp(base/'qmp.sock')
  serial=(base/'serial.log').read_text(errors='replace') if (base/'serial.log').exists() else ''
  assert 'PANIC_HEX:' not in serial and 'ECM_TEST_FAIL' not in serial,serial[-2500:]
  assert not errors,errors
  if 'ECM_NETWORK_INTEGRITY_OK' in serial:
   if args.send_concurrency:assert 'ECM_CONCURRENT_WRITERS_OK' in serial,serial[-2500:]
   if args.send_close_race:assert 'ECM_SEND_CLOSE_RACE_OK' in serial,serial[-2500:]
   if args.send_user_copy:assert 'ECM_USER_SEND_PATHS_OK' in serial,serial[-2500:]
   if args.tcp_flight2:assert 'two-packet TCP send flight enabled' in serial,serial[-2500:]
   if args.tcp_flight4:assert 'four-packet TCP send flight enabled' in serial,serial[-2500:]
   if args.tcp_adaptive_flight:assert 'bounded adaptive TCP send flight enabled' in serial,serial[-2500:]
   assert f'rx_depth={8 if args.rx_depth8 else 1}' in serial, 'Unexpected ECM receive depth'
   assert f'tx_depth={16 if args.tx_depth16 else 8}' in serial, 'Unexpected ECM transmit depth'
   break
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
  s.sendall(b'@72 bridge network\n');reply=b''
  while b'BRIDGE_END 72' not in reply and len(reply)<65536:
   chunk=s.recv(8192)
   if not chunk:break
   reply+=chunk
  assert b'BRIDGE_BEGIN 72' in reply and b'BRIDGE_END 72' in reply,reply
  line=next(line for line in reply.splitlines() if line.startswith(b'{'))
  report=json.loads(line);stats=report['network_driver_snapshot']
  assert report['tcp_ack_submit_failures']>=0,report
  assert report['tcp_recv_window_cap_bytes']==(16384 if args.tcp_window16 else 0),report
  assert stats['rx_depth']==(8 if args.rx_depth8 else 1),stats
  assert stats['tx_depth']==(16 if args.tx_depth16 else 8),stats
  assert stats['rx_completed']>0 and stats['tx_completed']>0 and not stats['failed'],stats
  assert 0<=stats['rx_queued']<=stats['rx_queue_high_water']<=32,stats
  (base/'network-snapshot.json').write_text(json.dumps(stats,indent=2)+'\n')
  if args.tcp_send_probe:
   s.sendall(b'@73 bridge tcp send-probe\n');reply=b''
   while b'BRIDGE_END 73' not in reply and len(reply)<65536:
    chunk=s.recv(8192)
    if not chunk:break
    reply+=chunk
   assert b'BRIDGE_BEGIN 73' in reply and b'BRIDGE_END 73' in reply,reply
   probe=json.loads(next(line for line in reply.splitlines() if line.startswith(b'{')))
   assert probe['enabled'] and probe['calls']>0 and probe['submitted_bytes']>0 and probe['ack_samples']>0,probe
   (base/'tcp-send-probe.json').write_text(json.dumps(probe,indent=2)+'\n')
 if loss:
  assert not loss['errors'] and loss['dropped']==1 and loss['retransmitted'],loss
 q.call('device_del',{'id':'usbnet'})
 q.call('human-monitor-command',{'command-line':'sendkey a 100'})
 deadline=time.monotonic()+10
 while time.monotonic()<deadline:
  serial=(base/'serial.log').read_text(errors='replace')
  if 'ECM_USB_STILL_ALIVE' in serial and 'HWKEY: down sc=0x1e' in serial:break
  time.sleep(.05)
 else:raise TimeoutError('USB/HID did not remain responsive after NIC removal')
 loss_stop.set()
 q.call('quit');assert owner.wait(timeout=15)==0
 if loss_thread:
  loss_thread.join(5);assert not loss_thread.is_alive(),'Packet relay did not stop'
  assert not loss['errors'],loss
  (base/'loss-result.json').write_text(json.dumps(loss,indent=2)+'\n')
 for thread in threads:thread.join(2)
 assert not errors and set(completed)=={'tcp','udp'},(errors,completed)
 sizes=[];windows=[];scales={};pending={};max_flight=0;raw=(base/'wire.pcap').read_bytes();at=24
 assert raw[:4]==b'\xd4\xc3\xb2\xa1'
 while at+16<=len(raw):
  _,_,captured,_=struct.unpack_from('<4I',raw,at);at+=16
  packet=raw[at:at+captured];at+=captured
  if len(packet)>=34 and packet[12:14]==b'\x08\x00':
   outbound=packet[26:30]==bytes([10,0,2,15]);inbound=packet[30:34]==bytes([10,0,2,15])
   if outbound:sizes.append(len(packet))
   ip_header=(packet[14]&15)*4;tcp_offset=14+ip_header
   if (outbound or inbound) and packet[23]==6 and len(packet)>=tcp_offset+20:
    src,dst=struct.unpack_from('!HH',packet,tcp_offset);flags=packet[tcp_offset+13]
    flow=(src,packet[30:34],dst) if outbound else (dst,packet[26:30],src)
    tcp_header=(packet[tcp_offset+12]>>4)*4
    ip_total=struct.unpack_from('!H',packet,16)[0]
    payload_len=max(0,ip_total-ip_header-tcp_header)
    if outbound and dst==port and payload_len:
     seq=struct.unpack_from('!I',packet,tcp_offset+4)[0]
     prior=pending.setdefault(flow,[])
     chain=1;previous=seq
     while chain<=len(prior):
      found=next(((start,end) for start,end in prior if end==previous),None)
      if found is None:break
      chain+=1;previous=found[0]
     max_flight=max(max_flight,chain)
     if not any(start==seq for start,_ in prior):prior.append((seq,(seq+payload_len)&0xffffffff))
    if inbound and src==port and flags&16:
     ack=struct.unpack_from('!I',packet,tcp_offset+8)[0]
     pending[flow]=[(start,end) for start,end in pending.get(flow,[]) if ((ack-end)&0xffffffff)>=0x80000000]
    if flags&2:
     hdr_len=(packet[tcp_offset+12]>>4)*4;opts=packet[tcp_offset+20:tcp_offset+hdr_len];i=0;scale=None
     while i<len(opts):
      kind=opts[i]
      if kind==0:break
      if kind==1:i+=1;continue
      if i+1>=len(opts) or opts[i+1]<2 or i+opts[i+1]>len(opts):break
      if kind==3 and opts[i+1]==3:scale=min(opts[i+2],14)
      i+=opts[i+1]
     scales.setdefault(flow,{})['guest' if outbound else 'peer']=scale
    if outbound:windows.append((flow,flags,struct.unpack_from('!H',packet,tcp_offset+14)[0]))
 assert 64 in sizes and 1024 in sizes,sorted(set(sizes))
 if args.tcp_flight2:assert max_flight>=2,'capture did not observe two successive payload segments before their ACK'
 if args.tcp_flight4:assert max_flight>=4,'capture did not observe four successive payload segments before their ACK'
 if args.tcp_adaptive_flight:assert max_flight>=4,'capture did not observe adaptive flight growth'
 if args.tcp_window16:
  assert any(flags&2 for _,flags,_ in windows) and any((flags&16) and not(flags&2) for _,flags,_ in windows),windows
  for flow,flags,window in windows:
   pair=scales.get(flow,{});shift=pair.get('guest') if pair.get('guest') is not None and pair.get('peer') is not None else 0
   assert (window if flags&2 else window<<shift)<=16384,(flow,flags,window,shift)
 result=dict(passed=True,rx_depth=8 if args.rx_depth8 else 1,tx_depth=16 if args.tx_depth16 else 8,tcp_window_cap=16384 if args.tcp_window16 else 0,tcp_flight2=args.tcp_flight2,tcp_flight4=args.tcp_flight4,tcp_adaptive_flight=args.tcp_adaptive_flight,network_delay_us=args.network_delay_us,max_flight_observed=max_flight,tcp_verified_bytes=262144,udp_verified_bytes=982,zero_length_termination_frames=[64,1024],
  dropped_tcp_packet=bool(args.drop_tcp_packet),idle_rx_seconds=1.5,bridge_framed_reply=True,usb_storage_concurrent=True,usb_storage_after_nic_removal=True,usb_keyboard_after_nic_removal=True)
 (base/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('USB_ECM_ACCEPTANCE_PASS',flush=True)
finally:
 loss_stop.set()
 if owner.poll() is None:
  try:
   if q:q.call('quit');owner.wait(timeout=10)
   else:owner.terminate();owner.wait(timeout=10)
  except BaseException:owner.kill();owner.wait(timeout=10)
 for connection in loss_connections:
  try:connection.shutdown(socket.SHUT_RDWR)
  except OSError:pass
 if loss_thread:loss_thread.join(5)
 tcp.close();udp.close();log.close()
 if loss_sockets:loss_sockets.cleanup()
