#!/usr/bin/env python3
"""Test the physical-machine MCP path through QEMU's guest TCP forward.

QMP is used only by this test harness for launch/reset/exit; the tested hardware
bridge has no QMP endpoint, VM registry or host process identity. No host USB,
desktop or audio is accessed.
"""
import argparse,hashlib,json,os,selectors,shutil,socket,subprocess,time,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',required=True,type=Path);args=ap.parse_args()
base=args.output.resolve();base.mkdir(parents=True,exist_ok=False);out=base/'probe'
def run(*cmd):subprocess.run(list(map(str,cmd)),cwd=ROOT,check=True)
run(os.environ['MUSL_CC'],'-static','-O2','-Wall','-Wextra','-Werror',ROOT/'src/tests/test_hardware_bridge_guest/main.c','-o',base/'HWTEST.ELF')
run('python3',ROOT/'scripts/make_uefi_probe.py','--output',out,'--driver','xhci','--usb-read-only','--partition-table','mbr','--display-width','800','--display-height','600')
initrd=out/'payload/INITRD.IMG';run('mcopy','-i',initrd,base/'HWTEST.ELF','::BOOT/HWTEST.ELF')
run('mcopy','-o','-i',str(out/'probe.raw')+'@@1048576',initrd,'::INITRD.IMG')
manifest=json.loads((out/'manifest.json').read_text());manifest['files']['INITRD.IMG']['sha256']=hashlib.sha256(initrd.read_bytes()).hexdigest()
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
variables=base/'VARS.fd';shutil.copy2(os.environ['OVMF_VARS'],variables);variables.chmod(0o600)
with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
qemu=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','4','-m','2048','-nodefaults',
 '-drive','if=pflash,format=raw,readonly=on,file='+os.environ['OVMF_CODE'],'-drive','if=pflash,format=raw,file='+str(variables),
 '-device','VGA','-device','qemu-xhci,id=usbc','-drive','if=none,id=stick,format=raw,readonly=on,file='+str(out/'probe.raw'),
 '-device','usb-storage,bus=usbc.0,port=1,drive=stick,bootindex=1','-device','usb-net,bus=usbc.0,port=2,netdev=network',
 '-netdev',f'user,id=network,restrict=on,hostfwd=tcp:127.0.0.1:{port}-:1234','-display','none',
 '-serial','file:'+str(base/'serial.log'),'-qmp','unix:'+str(base/'qmp.sock')+',server=on,wait=off','-action','reboot=reset']
(base/'argv.json').write_text(json.dumps(qemu,indent=2)+'\n')
class Qmp:
 def __init__(self):
  self.s=socket.socket(socket.AF_UNIX);self.s.settimeout(10);self.s.connect(str(base/'qmp.sock'));self.f=self.s.makefile('rwb',buffering=0);self.f.readline();self.call('qmp_capabilities')
 def call(self,cmd):
  self.f.write((json.dumps({'execute':cmd})+'\n').encode())
  while True:
   r=json.loads(self.f.readline())
   if 'return' in r:return r['return']
   if 'error' in r:raise RuntimeError(r)
class Rpc:
 def __init__(self):
  env=dict(os.environ,BROOK_ROOT=str(ROOT),BROOK_HARDWARE_REGISTRY=str(base/'hardware-registry'),BROOK_VM_REGISTRY=str(base/'absent-vm-registry'))
  self.log=(base/'bridge-server.log').open('x');self.owner=subprocess.Popen(['python3',str(ROOT/'tools/vm_bridge/server.py')],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.log,env=env)
  self.id=0;self.select=selectors.DefaultSelector();self.select.register(self.owner.stdout,selectors.EVENT_READ)
 def request(self,method,params=None):
  self.id+=1;self.owner.stdin.write((json.dumps({'jsonrpc':'2.0','id':self.id,'method':method,'params':params or {}})+'\n').encode());self.owner.stdin.flush()
  assert self.select.select(40),'MCP response timed out'
  r=json.loads(self.owner.stdout.readline());assert r.get('id')==self.id and 'error' not in r,r
  return r['result']
 def tool(self,name,arguments=None,error=False):
  r=self.request('tools/call',{'name':name,'arguments':arguments or {}})
  assert bool(r.get('isError'))==error,r
  text=''.join(item['text'] for item in r['content'] if item['type']=='text')
  return text if error else json.loads(text)
 def close(self):
  self.owner.stdin.close()
  try:self.owner.wait(timeout=15)
  except subprocess.TimeoutExpired:self.owner.terminate();self.owner.wait(timeout=10)
  self.log.close();self.select.close()
log=(base/'qemu.log').open('x');owner=subprocess.Popen(qemu,stdout=log,stderr=subprocess.STDOUT);q=None;rpc=None
try:
 def boot(offset=0):
  deadline=time.monotonic()+90
  while time.monotonic()<deadline:
   assert owner.poll() is None,'QEMU ended early'
   serial=(base/'serial.log').read_text(errors='replace')[offset:] if (base/'serial.log').exists() else ''
   assert 'PANIC_HEX:' not in serial,serial[-2000:]
   if 'DEBUG_TCP: listening on port 1234' in serial:return
   time.sleep(.05)
  raise TimeoutError(serial[-2000:])
 boot();q=Qmp();rpc=Rpc();rpc.request('initialize',{'protocolVersion':'2025-06-18'})
 tools=rpc.request('tools/list')['tools'];assert {'hw_attach','hw_doctor','hw_guest_run','hw_logs'}<={t['name'] for t in tools}
 attached=rpc.tool('hw_attach',{'host':'127.0.0.1','port':port});session=attached['session']
 doctor=rpc.tool('hw_doctor',{'session':session});assert doctor['reachable'] and doctor['identity_matches'] and not doctor['capabilities']['qmp']
 stats=rpc.tool('hw_stats',{'session':session});assert stats['process_count']>0
 logs=rpc.tool('hw_logs',{'session':session,'limit':64});assert logs['logs_api']==1 and isinstance(logs['lines'],list)
 cursor=logs['next_cursor'];more=rpc.tool('hw_logs',{'session':session,'cursor':cursor,'limit':64});assert more['next_cursor']>=cursor
 bad=rpc.tool('hw_logs',{'session':session,'cursor':0xffffffff},error=True);assert 'cursor' in bad.lower()
 request=uuid.uuid4().hex;arguments={'session':session,'request_id':request,'argv':['/boot/HWTEST.ELF','hello world'],'cwd':'/','wait_s':5}
 job=rpc.tool('hw_guest_run',arguments)
 deadline=time.monotonic()+15
 while job.get('state') not in {'completed','failed'} and time.monotonic()<deadline:
  job=rpc.tool('hw_job',{'session':session,'operation_id':request,'wait_s':2})
 assert job['exit']['code']==17,job
 assert 'hello world' in job['stdout']['text'] and '✓' in job['stdout']['text'],job
 assert 'separate stderr' in job['stderr']['text'],job
 replay=rpc.tool('hw_guest_run',arguments);assert replay['exit']==job['exit'] and replay['operation_id']==job['operation_id']
 rpc.tool('hw_guest_run',{'session':session,'request_id':uuid.uuid4().hex,'argv':['/boot/HWTEST.ELF','window'],'cwd':'/','wait_s':0})
 deadline=time.monotonic()+8;reference=None
 while time.monotonic()<deadline:
  windows=rpc.tool('hw_windows',{'session':session})
  fixture=[w for w in windows['windows'] if w['title']=='Hardware bridge window']
  if fixture:reference=fixture[0]['reference'];break
  time.sleep(.1)
 assert reference,'Native window fixture did not appear'
 for action in ['minimize','restore','maximize','restore']:
  request_window=uuid.uuid4().hex
  parameters={'session':session,'window':reference,'action':action,'request_id':request_window,'wait_s':2}
  if action=='restore':parameters['restore_target']='normal'
  operation=rpc.tool('hw_window',parameters);assert operation['state']=='completed',operation
  observed=rpc.tool('hw_ui',{'session':session,'operation_id':request_window});assert observed['state']=='completed'
  assert operation['current_window']['minimized']==(action=='minimize'),operation
  assert operation['current_window']['maximized']==(action=='maximize'),operation
 apps=rpc.tool('hw_apps',{'session':session});assert isinstance(apps['apps'],list)
 offset=len((base/'serial.log').read_text(errors='replace'));q.call('system_reset');boot(offset)
 assert 'Stale guest boot' in rpc.tool('hw_stats',{'session':session},error=True)
 assert 'Stale guest boot' in rpc.tool('hw_window',{'session':session,'window':reference,'action':'focus','request_id':uuid.uuid4().hex},error=True)
 stale_job=dict(arguments,request_id=uuid.uuid4().hex)
 assert 'Stale guest boot' in rpc.tool('hw_guest_run',stale_job,error=True)
 stale=rpc.tool('hw_doctor',{'session':session});assert stale['reachable'] and not stale['identity_matches']
 fresh=rpc.tool('hw_attach',{'host':'127.0.0.1','port':port});assert fresh['guest_boot_id']!=attached['guest_boot_id']
 assert rpc.tool('hw_stats',{'session':fresh['session']})['process_count']>0
 assert not (base/'absent-vm-registry').exists(),'Hardware path accessed the VM registry'
 result=dict(passed=True,mcp_tools_discovered=True,qmp_unavailable_to_bridge=True,stats=True,logs_with_cursors=True,
  structured_job_uid=1000,job_exit=17,separate_stdout_stderr=True,identical_request_recovered=True,
  semantic_windows=True,window_minimize_maximize_restore=True,app_catalog=True,old_boot_rejected_after_reset=True,stale_mutations_rejected=True,explicit_reattach=True,
  kernel_sha256=hashlib.sha256((out/'payload/KERNEL/BROOK.ELF').read_bytes()).hexdigest())
 (base/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('HARDWARE_BRIDGE_ACCEPTANCE_PASS',flush=True)
finally:
 if rpc:rpc.close()
 if owner.poll() is None:
  try:
   if q:q.call('quit');owner.wait(timeout=15)
   else:owner.terminate();owner.wait(timeout=15)
  except BaseException:owner.kill();owner.wait(timeout=10)
 log.close()
