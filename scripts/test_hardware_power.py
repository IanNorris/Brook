#!/usr/bin/env python3
"""Test physical-bridge graceful halt and firmware reset inside disposable QEMU.

Run in nix-shell with a prepared offline-desktop probe directory. Physical USB,
real host power controls, host desktop and speakers are never accessed. The
source image stays unchanged; reset is requested only through the MCP tool.
"""
import argparse,hashlib,json,os,selectors,shutil,socket,subprocess,time,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--probe',type=Path,required=True)
ap.add_argument('--output',type=Path,required=True)
args=ap.parse_args();output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
class Qmp:
 def __init__(self,path):
  self.s=socket.socket(socket.AF_UNIX);self.s.settimeout(10);self.s.connect(str(path));self.f=self.s.makefile('rwb',buffering=0);self.f.readline();self.call('qmp_capabilities')
 def call(self,command):
  self.f.write((json.dumps({'execute':command})+'\n').encode())
  while True:
   reply=json.loads(self.f.readline())
   if 'return' in reply:return reply['return']
   if 'error' in reply:raise RuntimeError(reply)
 def events(self):
  rows=[]
  while True:
   line=self.f.readline()
   if not line:return rows
   rows.append(json.loads(line))
 def close(self):self.f.close();self.s.close()
class Rpc:
 def __init__(self,base):
  env=dict(os.environ,BROOK_ROOT=str(ROOT),BROOK_HARDWARE_REGISTRY=str(base/'registry'),BROOK_VM_REGISTRY=str(base/'absent-vm-registry'))
  self.log=(base/'mcp.log').open('x');self.owner=subprocess.Popen(['python3',str(ROOT/'tools/vm_bridge/server.py')],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.log,env=env)
  self.id=0;self.select=selectors.DefaultSelector();self.select.register(self.owner.stdout,selectors.EVENT_READ)
 def request(self,method,params=None):
  self.id+=1;self.owner.stdin.write((json.dumps({'jsonrpc':'2.0','id':self.id,'method':method,'params':params or {}})+'\n').encode());self.owner.stdin.flush()
  assert self.select.select(20),'MCP timeout';reply=json.loads(self.owner.stdout.readline());assert reply.get('id')==self.id and 'error' not in reply,reply;return reply['result']
 def tool(self,name,arguments):
  reply=self.request('tools/call',{'name':name,'arguments':arguments});assert not reply.get('isError'),reply
  return json.loads(''.join(item['text'] for item in reply['content'] if item['type']=='text'))
 def close(self):
  self.owner.stdin.close()
  try:self.owner.wait(timeout=10)
  except subprocess.TimeoutExpired:self.owner.terminate();self.owner.wait(timeout=10)
  self.log.close();self.select.close()
results=[]
for operation in ['shutdown','restart','q35-stop']:
 base=output/operation;base.mkdir();disk=base/'probe.raw'
 subprocess.run(['cp','--reflink=auto',str(args.probe.resolve()/'probe.raw'),str(disk)],check=True)
 media=str(disk)+'@@1048576'
 for source,target in [('build/release/kernel/BROOK.elf','::KERNEL/BROOK.ELF'),('build/release/bootloader/BOOTX64.efi','::EFI/BOOT/BOOTX64.EFI')]:
  subprocess.run(['mcopy','-o','-i',media,str(ROOT/source),target],check=True)
 variables=base/'VARS.fd';shutil.copy2(os.environ['OVMF_VARS'],variables);variables.chmod(0o600)
 with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
 argv=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','4','-m','4096','-nodefaults',
  '-drive','if=pflash,format=raw,readonly=on,file='+os.environ['OVMF_CODE'],'-drive','if=pflash,format=raw,file='+str(variables),
  '-device','VGA','-device','qemu-xhci,id=usbc','-drive','if=none,id=stick,format=raw,readonly=on,file='+str(disk),
  '-device','usb-storage,bus=usbc.0,port=1,drive=stick,bootindex=1','-device','usb-net,bus=usbc.0,port=2,netdev=network',
  '-netdev',f'user,id=network,restrict=on,hostfwd=tcp:127.0.0.1:{port}-:1234','-display','none','-serial','file:'+str(base/'serial.log'),
  '-qmp','unix:'+str(base/'qmp.sock')+',server=on,wait=off','-no-reboot']
 if operation=='q35-stop':argv.extend(['-fw_cfg','name=opt/brook/power,string=q35-stop'])
 # Other cases omit opt/brook/power: reset must come from validated FADT.
 (base/'argv.json').write_text(json.dumps(argv,indent=2)+'\n')
 log=(base/'qemu.log').open('x');owner=subprocess.Popen(argv,stdout=log,stderr=subprocess.STDOUT);q=None;rpc=None
 try:
  deadline=time.monotonic()+90
  while time.monotonic()<deadline:
   assert owner.poll() is None,'QEMU exited before boot'
   serial=(base/'serial.log').read_text(errors='replace') if (base/'serial.log').exists() else ''
   assert 'PANIC_HEX:' not in serial,serial[-2000:]
   if 'USB_IO_PROBE_OK' in serial and 'DEBUG_TCP: listening' in serial:break
   time.sleep(.1)
  else:raise TimeoutError(serial[-3000:])
  q=Qmp(base/'qmp.sock');rpc=Rpc(base);init=rpc.request('initialize',{'protocolVersion':'2025-06-18'});assert init['serverInfo']['version']=='0.11.0'
  assert 'hw_power' in {row['name'] for row in rpc.request('tools/list')['tools']}
  attach=rpc.tool('hw_attach',{'host':'127.0.0.1','port':port});session=attach['session']
  doctor=rpc.tool('hw_doctor',{'session':session});caps=doctor['capabilities']['power']
  if operation=='q35-stop':
   assert caps['graceful_quiescence'] and not caps['reset'] and caps['power_off'] and caps['reset_method']=='disabled',doctor
  else:assert caps['graceful_quiescence'] and caps['reset'] and not caps['power_off'] and caps['reset_method']=='acpi-io8',doctor
  (base/'doctor.json').write_text(json.dumps(doctor,indent=2)+'\n')
  if operation=='shutdown':
   catalog=rpc.tool('hw_apps',{'session':session})
   app=next(row for row in catalog['apps'] if 'mousepad' in row['title'].lower() and row['launch_eligible'])
   launch_id=uuid.uuid4().hex
   launched=rpc.tool('hw_launch',dict(session=session,app=app['reference'],request_id=launch_id,wait_for='window',wait_s=5))
   end=time.monotonic()+30
   while not launched.get('wait_condition_met') and time.monotonic()<end:
    assert launched['state'] not in ['failed','rejected','lost'],launched
    launched=rpc.tool('hw_ui',dict(session=session,operation_id=launch_id,wait_for='window',wait_s=2))
   assert launched.get('wait_condition_met'),launched
   (base/'launcher.json').write_text(json.dumps(dict(catalog=catalog,launched=launched),indent=2)+'\n')
  request=uuid.uuid4().hex;power_args=dict(session=session,request_id=request)
  if operation=='q35-stop':
   refused=rpc.tool('hw_power',{**power_args,'operation':'restart'});assert refused['state']=='unsupported' and refused['error']==-95,refused
  started=rpc.tool('hw_power',{**power_args,'operation':'shutdown' if operation=='q35-stop' else operation});assert started['state']=='closing',started
  # Same ID is a status observation from here, never replay the mutation.
  states=[];deadline=time.monotonic()+90
  while time.monotonic()<deadline:
   status=rpc.tool('hw_power',{**power_args,'operation':'status'});states.append(status)
   assert status['state']!='failed',status
   if status['state']=='ready':break
   time.sleep(.1)
  else:raise TimeoutError(states[-1])
  (base/'states.json').write_text(json.dumps(states,indent=2)+'\n')
  committed=rpc.tool('hw_power',{**power_args,'operation':'commit'})
  if operation=='shutdown':
   assert status['status']=='Safe to power off manually' and committed['state']=='ready' and committed['error']==-95,committed
   assert owner.poll() is None
   assert rpc.tool('hw_stats',{'session':session})['process_count']>0
   q.call('quit');assert owner.wait(timeout=15)==0
   result=dict(operation=operation,passed=True,manual_power_off=True,unsupported_commit_preserves_ready=True)
  else:
   assert committed['error']==0,committed
   assert owner.wait(timeout=20)==0
   events=q.events();(base/'qmp-events.json').write_text(json.dumps(events,indent=2)+'\n')
   expected='guest-reset' if operation=='restart' else 'guest-shutdown'
   assert any(row.get('event')=='SHUTDOWN' and row.get('data',{}).get('reason')==expected for row in events),events
   result=dict(operation=operation,passed=True,firmware_reset=operation=='restart',q35_power_opt_in=operation=='q35-stop')
  results.append(result);print('HARDWARE_POWER_CASE_PASS',operation,flush=True)
 finally:
  if rpc:rpc.close()
  if q:q.close()
  if owner.poll() is None:
   owner.terminate()
   try:owner.wait(timeout=10)
   except subprocess.TimeoutExpired:owner.kill();owner.wait()
  log.close()
(output/'result.json').write_text(json.dumps(dict(passed=True,cases=results,kernel_sha256=hashlib.sha256((ROOT/'build/release/kernel/BROOK.elf').read_bytes()).hexdigest()),indent=2)+'\n')
print('HARDWARE_POWER_ACCEPTANCE_PASS',output,flush=True)
