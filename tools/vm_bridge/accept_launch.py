#!/usr/bin/env python3
"""Real normal-launcher activation and durable recovery on an empty disposable VM."""
import argparse,json,time,uuid
from pathlib import Path
import server

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--session',required=True);p.add_argument('--out',required=True,type=Path)
p.add_argument('--negative-tests',action='store_true',help='Temporarily edit/restore the application manifest on disposable disks to test failed dispatch')
a=p.parse_args();s=a.session;out=a.out.resolve()
if not out.is_relative_to((server.ROOT/'build').resolve()):raise ValueError('Keep acceptance evidence under build/')
out.mkdir(parents=True,exist_ok=True);evidence={}
def save(): (out/'launch.json').write_text(json.dumps(evidence,indent=2))
def dismiss_menu():
 f=server.observe(s)
 if (f['data']['width'],f['data']['height'])!=(1920,1080):raise ValueError('Pixel fixture requires 1920x1080 at 100% scale')
 server.input_actions(s,[{'type':'click','x':1900,'y':1000,'button':'left'}],frame_id=f['data']['frame_id'])
def activate(app,key):
 ident=uuid.uuid4().hex;start=time.monotonic()
 r=server.launch_app(s,app['reference'],ident,wait_s=15)
 if not r['wait_condition_met'] and r['state'] not in {'failed','rejected','cancelled','lost'}:
  r=server.ui_status(s,ident,wait_for='window',wait_s=15)
 evidence[key]={'operation_id':ident,'result':r,'elapsed_s':time.monotonic()-start};save()
 assert r['wait_condition_met'],(key,r)
 assert r['launch']['dispatch_finished'] and r['launch']['dispatch_succeeded'],r
 assert r['current_window']['client_origin']!='unknown',r
 # A separate Ui object reloads the durable registry. Repeated submit must not
 # create a different token or enqueue a second activation.
 again=server.launch_app(s,app['reference'],ident,wait_s=0)
 assert again['launch']['token']==r['launch']['token']
 evidence[key]['recovered']=again;save()
 print('LAUNCHED',key,round(time.monotonic()-start,3),r['launch']['correlation'],flush=True)
 return r
if server.windows(s)['windows']:raise ValueError('Start with an empty disposable desktop')
dismiss_menu()
catalog=server.apps(s);evidence['catalog']=catalog;save()
terminal=next(x for x in catalog['apps'] if x['title']=='Terminal')
editor=next(x for x in catalog['apps'] if x['desktop_app_id']=='org.xfce.mousepad')
native=activate(terminal,'native');wayland=activate(editor,'wayland')
assert native['current_window']['client_origin']=='native_process'
assert wayland['current_window']['client_origin']=='provider_registered_unix_origin'
# This may find an existing window before a new window appears; it is not
# proof of activation, single-instance handoff or a required process count.
# Preserve precisely what the guest observed.
second=activate(editor,'wayland_second_request')
# Allow later exact launch-tag evidence to strengthen an earlier app-ID match.
time.sleep(.5)
evidence['second_request_later_status']=server.ui_status(s,evidence['wayland_second_request']['operation_id'],wait_for='window',wait_s=0);save()
frame=server.observe(s);(out/'semantic-launches.png').write_bytes(frame['image'])
evidence['capture']=frame['data'];save()
# Semantic actions use the same references; close each actual remaining window once.
for row in server.windows(s)['windows']:
 r=server.window_action(s,row['reference'],'close',uuid.uuid4().hex,wait_s=10,wait_for='closed')
 evidence.setdefault('closes',[]).append(r);save();assert r['wait_condition_met'],r
# Refresh through the real menu, then dispatch the old catalog reference.
f=server.observe(s)
server.input_actions(s,[{'type':'click','x':20,'y':1058,'button':'left'}],frame_id=f['data']['frame_id'])
deadline=time.monotonic()+15
while server.apps(s)['catalog_generation']==catalog['catalog_generation']:
 assert time.monotonic()<deadline
 time.sleep(.1)
stale=server.launch_app(s,editor['reference'],uuid.uuid4().hex,wait_s=2)
evidence['stale_catalog']=stale;save()
assert stale['state']=='rejected' and stale['guest']['error_code']==-116,stale
# Host takeover must reject a new mutation before it reaches the guest queue.
evidence['takeover']=server.control(s,'takeover');save()
try:
 fresh=server.apps(s);app=next(x for x in fresh['apps'] if x['desktop_app_id']=='org.xfce.mousepad')
 blocked=server.launch_app(s,app['reference'],uuid.uuid4().hex,wait_s=0)
 evidence['blocked']=blocked;save();assert blocked['state']=='rejected' and 'takeover' in blocked.get('error',''),blocked
finally:
 evidence['release']=server.control(s,'release');save()
if a.negative_tests:
 manifest='/nix/share/applications/applications.idx';backup='/tmp/brook-launch-backup.idx'
 probe='/tmp/launch-probe-'+uuid.uuid4().hex+'.elf'
 def job(source,*args):
  r=server.guest_run(s,argv=['/boot/BIN/BASH','-c',source,'launch-test',*args],uid=0,request_id=uuid.uuid4().hex,wait_s=20)
  assert r['state']=='completed' and r['exit']['code']==0,r
 def refresh():
  dismiss_menu()
  old=server.apps(s)['catalog_generation'];f=server.observe(s)
  server.input_actions(s,[{'type':'click','x':20,'y':1058,'button':'left'}],frame_id=f['data']['frame_id'])
  deadline=time.monotonic()+15
  while True:
   result=server.apps(s)
   if result['catalog_generation']!=old:
    dismiss_menu();return result
   assert time.monotonic()<deadline
   time.sleep(.1)
 job('/boot/BIN/busybox cp "$1" "$2"',manifest,backup)
 try:
  row='Launch failure probe\t'+probe+'\t\t\tbrook.launch.probe\n'
  job('/boot/BIN/busybox cp /boot/BIN/HELLO "$1"; printf "%s" "$2" >> "$3"',probe,row,manifest)
  fresh=refresh();app=next(x for x in fresh['apps'] if x['desktop_app_id']=='brook.launch.probe')
  job('/boot/BIN/busybox rm "$1"',probe)
  result=server.launch_app(s,app['reference'],uuid.uuid4().hex,wait_s=5)
  evidence['removed_executable']=result;save()
  assert result['state']=='failed' and result['launch']['raw_dispatch_result']==-2,result
 finally:
  job('/boot/BIN/busybox cp "$1" "$2"',backup,manifest)
  evidence['restored_catalog']=refresh();save()
 print('REMOVED EXECUTABLE / RAW DISPATCH FAILURE PASS',flush=True)
print('ACCEPTED native / Wayland / recovered IDs / second request / stale catalog / takeover',flush=True)
