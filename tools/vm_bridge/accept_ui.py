#!/usr/bin/env python3
"""Validate semantic controls on a disposable explicit VM with Terminal and Mousepad already open.
Both apps are closed; use clean documents. Launch/setup is intentionally separate
from these controls and is not a semantic Applications launcher acceptance.
"""
import json,uuid,time
from pathlib import Path
import server
import argparse
parser=argparse.ArgumentParser(description='Exercise existing native Terminal and clean Mousepad windows, then gracefully close both; explicit-session acceptance only.')
parser.add_argument('--session',required=True)
parser.add_argument('--out',type=Path,required=True)
args=parser.parse_args()
out=args.out.resolve()
if not out.is_relative_to((server.ROOT/'build').resolve()):raise ValueError('Keep acceptance evidence below build/')
out.mkdir(parents=True,exist_ok=True)
s=args.session;records=[]
def save(): (out/'actions.json').write_text(json.dumps(records,indent=2))
def action(row,verb,**kw):
 ident=uuid.uuid4().hex;start=time.monotonic()
 r=server.window_action(s,row['reference'],verb,ident,wait_s=15,**kw)
 if not r['wait_condition_met'] and r['state']!='rejected':
  time.sleep(1)
  r=server.ui_status(s,ident,wait_s=15,wait_for=kw.get('wait_for','applied'))
 records.append({'action':verb,'target':row['reference'],'result':r,'elapsed_s':time.monotonic()-start});save()
 assert r['state']=='completed',(verb,r)
 assert r['wait_condition_met'],(verb,r)
 print(row['title'],verb,round(time.monotonic()-start,3),flush=True)
 return r,ident
deadline=time.monotonic()+60
while True:
 rows=server.windows(s)['windows']
 if any(x['app_id']=='org.xfce.mousepad' for x in rows):break
 assert time.monotonic()<deadline,rows
 time.sleep(.2)
native=next(r for r in rows if r['client_origin']=='native_process' and r['title']=='Terminal');wayland=next(r for r in rows if r['app_id']=='org.xfce.mousepad')
assert native['client']==native['provider'];assert wayland['client']['pid']!=wayland['provider']['pid']
for row in [native,wayland]:
 r,ident=action(row,'focus');assert r['current_window']['focused']
 r,ident=action(row,'minimize');assert r['current_window']['minimized']
 r,ident=action(row,'maximize');assert r['current_window']['minimized'] and r['current_window']['maximized']
 r,ident=action(row,'restore',restore_target='maximized');assert r['current_window']['maximized'] and not r['current_window']['minimized']
 # Idempotent resubmission is evidence recovery, with no second enqueue.
 again=server.window_action(s,row['reference'],'restore',ident,restore_target='maximized',wait_s=2)
 assert again['guest']['queued_tick_ms']==r['guest']['queued_tick_ms']
 deadline=time.monotonic()+10
 while True:
  snap=server.windows(s);current=next(x for x in snap['windows'] if x['reference']==row['reference'])
  if not current['api_window'] or (current['buffer_width']==current['width'] and current['buffer_height']==current['height']):break
  assert time.monotonic()<deadline,current
  time.sleep(.1)
 r,ident=action(row,'restore',restore_target='normal');assert not r['current_window']['maximized']
# Capture without serializing any image-bearing envelope.
frame=server.observe(s);(out/'native-and-wayland.png').write_bytes(frame['image'])
records.append({'capture':{k:v for k,v in frame.items() if k!='image'}});save()
r,ident=action(wayland,'close',wait_for='client_group_exit')
assert r['conditions']['closed']
# Verify an unrelated native app survives, and waylandd remains alive.
assert any(x['reference']==native['reference'] for x in server.windows(s)['windows'])
with server.session_lock(s) as d:
 live=server.guest(d,f"bridge ui process {wayland['provider']['pid']} {wayland['provider']['incarnation']} {r['guest_boot_id']}")
assert live['live_group_tasks']>0
r,ident=action(native,'close',wait_for='client_group_exit')
assert r['conditions']['closed']
print('ACCEPTED',len(records),'records',flush=True)
