#!/usr/bin/env python3
"""DESTRUCTIVE acceptance: reboot an empty agent VM with cloned disks; optional panic.
Uses QMP set-action to override experiment -no-reboot. Host session generation
intentionally stays unchanged to exercise the guest boot-epoch boundary.
"""
import argparse,json,time
from pathlib import Path
import server
from media import Delivery

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--session',required=True);p.add_argument('--out',required=True,type=Path)
p.add_argument('--panic',action='store_true',help='After reboot checks, sync then trigger a controlled panic; leave VM panicked for explicit stop')
a=p.parse_args();s=a.session;out=a.out.resolve()
if not out.is_relative_to((server.ROOT/'build').resolve()):raise ValueError('Keep acceptance evidence under build/')
out.mkdir(parents=True,exist_ok=True);data=server.load_session(s)
if data.get('provenance')!='agent' or not data.get('cloned_disks'):raise ValueError('Requires an agent VM with private cloned disks')
if server.windows(s)['windows']:raise ValueError('Close all applications before destructive acceptance')
evidence={}
def save(): (out/'reboot.json').write_text(json.dumps(evidence,indent=2))
ui=server.window_ui();_,caps=ui.capabilities(s);old=caps['boot_id'];generation=data['generation']
# Retain genuine pre-reboot metadata, including which lease token it came from.
f=server.observe(s,include=['frame']);safe=Delivery(server.REGISTRY,s).adapt(f)
assert f['state']=='completed',safe
evidence['before']=safe
cached=json.loads((server.REGISTRY/s/'frame-source.json').read_text());assert cached['guest_boot_id']==old
# Request another live lease and leave it held across reset.
def frame(boot,verb):
 with server.session_lock(s) as d:return server.guest(d,f'bridge frame guarded {boot} {verb}')
evidence['abandoned_old_lease']=frame(old,'request');save()
with server.session_lock(s) as d:
 sync=server.guest(d,'bridge sync',timeout=30);assert sync.get('guest_sync_completed') is True
 evidence['sync_before_reset']=sync;offset=Path(d['serial_log']).stat().st_size
 with server.QMP(s) as q:
  q.call('set-action',{'reboot':'reset'})
  q.call('system_reset');q.call('cont')
save();deadline=time.monotonic()+120
while True:
 assert server.process_is_alive(data['pid'],data['process_identity']),'VM process exited during reset'
 if 'DEBUG_TCP: listening on port 1234' in Path(data['serial_log']).read_bytes()[offset:].decode(errors='replace'):
  try:
   with server.session_lock(s) as d:server.guest(d,'uptime',timeout=3)
   break
  except OSError:pass
 assert time.monotonic()<deadline,'Guest reboot deadline'
 time.sleep(.5)
assert server.load_session(s)['generation']==generation
_,caps=ui.capabilities(s);new=caps['boot_id'];assert new!=old
evidence['new_boot_id']=new;evidence['old_boot_id']=old
fresh=frame(new,'request');assert fresh['token']==1,fresh
deadline=time.monotonic()+15
while fresh['state']==1:
 assert time.monotonic()<deadline
 time.sleep(.05);fresh=frame(new,'status 1')
assert fresh['state']==2,fresh;evidence['fresh_lease']=fresh
for verb in ['request','status 1','release 1']:
 stale=frame(old,verb);evidence['stale_'+verb.replace(' ','_')]=stale
 assert stale.get('frame_error')=='Stale guest boot',stale
assert frame(new,'status 1')['state']==2
assert frame(new,'release 1')['released'] is True
# Guarded low-level requests did not replace the prior boot's cached geometry.
assert json.loads((server.REGISTRY/s/'frame-source.json').read_text())['guest_boot_id']==old
save();print('PASSED same host generation / fresh guest epoch / reused frame token 1 / stale request-status-release isolation',flush=True)
if a.panic:
 with server.session_lock(s) as d:
  sync=server.guest(d,'bridge sync',timeout=30);assert sync.get('guest_sync_completed') is True
  evidence['sync_before_panic']=sync
 try:evidence['panic_request']=server.control(s,'panic')
 except (OSError,RuntimeError) as error:evidence['panic_request']={'delivery':'unknown','error':str(error)}
 save();deadline=time.monotonic()+30
 while not server.panic_state(data):
  assert time.monotonic()<deadline,'Panic deadline'
  time.sleep(.1)
 original=server.guest;commands=[]
 def traced(*args,**kw):commands.append(args[1]);return original(*args,**kw)
 server.guest=traced
 try:r=server.observe(s,include=['frame','windows','processes','launches','errors'])
 finally:server.guest=original
 evidence['panic_observation']=Delivery(server.REGISTRY,s).adapt(r);evidence['panic_guest_commands']=commands;save()
 assert commands==[],commands
 assert r['state']=='partial' and r['components']['frame']['state']=='unverified',evidence['panic_observation']
 assert r['components']['frame']['result']['data']['geometry_provenance']['guest_boot_id']==old
 for key in ['windows','processes','launches']:assert r['components'][key]['state']=='unavailable'
 assert any(e['classification']=='panic' for e in r['components']['errors']['result']['events'])
 (out/'panic-frame-unverified-geometry.png').write_bytes(r['components']['frame']['result']['image'])
 print('PASSED panic subset / no guest RPC / prior-boot geometry explicitly unverified / panic diagnostic',flush=True)
