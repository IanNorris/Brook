#!/usr/bin/env python3
"""Explicit-session app discovery/pixel-launch acceptance; use disposable VM disks.
The optional negative cases replace and restore the application manifest.
"""
import argparse,json,uuid,time,math
from pathlib import Path
import server
parser=argparse.ArgumentParser(description='Test catalog discovery and a real Mousepad menu click on a disposable, empty 1920x1080 desktop at 100% scale.')
parser.add_argument('--session',required=True)
parser.add_argument('--out',type=Path,required=True)
parser.add_argument('--negative-tests',action='store_true',help='Temporarily modify the guest application manifest as maintenance UID 0; restore it in finally. Use only disposable images.')
args=parser.parse_args()
evidence=args.out.resolve()
if not evidence.is_relative_to((server.ROOT/'build').resolve()):raise ValueError('Keep evidence below build/')
evidence.mkdir(parents=True,exist_ok=True)
s=args.session;records={};menu_open=False
if server.windows(s)['windows']:raise ValueError('Start with an empty disposable desktop')
frame=server.observe(s)
if (frame['data']['width'],frame['data']['height'])!=(1920,1080):raise ValueError('This pixel acceptance requires 1920x1080 at 100% desktop scale')

def save(): (evidence/'apps.json').write_text(json.dumps(records,indent=2))
def job(source,*args,uid=1000):
 r=server.guest_run(s,argv=['/boot/BIN/BASH','-c',source,'catalog-test',*args],uid=uid,request_id=uuid.uuid4().hex,wait_s=20)
 assert r['state']=='completed' and r['exit']['code']==0,r
 return r

def click(x,y):
 f=server.observe(s)
 return server.input_actions(s,[{'type':'click','x':x,'y':y,'button':'left','hold_seconds':.2}],frame_id=f['data']['frame_id'])
def open_menu():
 global menu_open
 if menu_open:click(20,1058);menu_open=False
 before=server.apps(s)['catalog_generation'];click(20,1058);menu_open=True
 deadline=time.monotonic()+15
 while True:
  catalog=server.apps(s)
  if catalog['catalog_generation']>before:return catalog
  assert time.monotonic()<deadline,('refresh timeout',catalog)
  time.sleep(.1)
def item_xy(catalog,index):
 # 100% desktop scale, 1920x1080 captured frame. The screenshot is also
 # retained to check the visible hitbox rather than claiming a semantic launch.
 count=len(catalog['apps']);cols=math.ceil(count/14);rows=math.ceil(count/cols)
 height=52+rows*42;y=1036-height-2
 return 10+(index%cols)*234+115,y+46+(index//cols)*42+20

base=server.apps(s);records['base']=base;save()
assert base['catalog_ready'] and base['catalog_generation']>0
if len(base['apps'])>40:raise ValueError('This fixed pixel fixture supports at most 40 initial entries without scrolling')
if any(x['desktop_app_id'] in {'probe.one','probe.two'} for x in base['apps']):raise ValueError('Test desktop IDs already present')
assert len({x['app_id'] for x in base['apps']})==len(base['apps'])
assert all(x['launch_eligible'] for x in base['apps'])
new=open_menu();records['refreshed']=new;save()
assert [(x['app_id'],x['title']) for x in base['apps']]==[(x['app_id'],x['title']) for x in new['apps']]
f=server.observe(s);(evidence/'applications-menu.png').write_bytes(f['image'])
# A real installed GTK app through the Applications menu, not a shell surrogate.
index=next(i for i,x in enumerate(new['apps']) if x['desktop_app_id']=='org.xfce.mousepad')
records['mousepad_click']=click(*item_xy(new,index));menu_open=False;save()
end=time.monotonic()+60
while True:
 matches=[x for x in server.windows(s)['windows'] if x['app_id']=='org.xfce.mousepad']
 if matches:break
 assert time.monotonic()<end
 time.sleep(.2)
records['mousepad_window']=matches[0];save()
assert matches[0]['launch_id']>0 and matches[0]['client_origin']=='provider_registered_unix_origin'
records['mousepad_close']=server.window_action(s,matches[0]['reference'],'close',uuid.uuid4().hex,wait_for='client_group_exit',wait_s=10)
assert records['mousepad_close']['wait_condition_met'];save()
print('CATALOG REFRESH / STABLE IDS / REAL MOUSEPAD MENU LAUNCH PASS',flush=True)

if not args.negative_tests:
 print('ACCEPTED; negative manifest cases not requested',flush=True)
 raise SystemExit(0)

manifest='/nix/share/applications/applications.idx';backup='/tmp/brook-apps-original.idx'
job('/boot/BIN/busybox cp "$1" "$2"',manifest,backup,uid=0)
probe='/tmp/catalog-probe-'+uuid.uuid4().hex+'.elf'
rows='Brook catalog probe\t'+probe+'\t\t\tprobe.one\nBrook catalog probe\t'+probe+'\t\t\tprobe.two\n'
try:
 job('/boot/BIN/busybox cp /boot/BIN/HELLO "$3"; /boot/BIN/busybox cp "$1" "$2"; printf "%s" "$4" >> "$2"',backup,manifest,probe,rows,uid=0)
 probes=open_menu();records['equal_titles']=probes;save()
 found=[x for x in probes['apps'] if x['title']=='Brook catalog probe']
 assert len(found)==2 and len({x['app_id'] for x in found})==2
 # Duplicate the ID as well as title. Publication must reject the entire
 # candidate and keep the old generation intact.
 duplicate=rows.replace('probe.two','probe.one')
 job('/boot/BIN/busybox cp "$1" "$2"; printf "%s" "$3" >> "$2"',backup,manifest,duplicate,uid=0)
 serial=Path(server.load_session(s)['serial_log']);offset=serial.stat().st_size
 click(20,1058);menu_open=False;click(20,1058);menu_open=True
 end=time.monotonic()+15
 while True:
  rejected_log=serial.read_bytes()[offset:].decode(errors='replace')
  if 'duplicate/colliding launcher identity' in rejected_log:break
  assert time.monotonic()<end,rejected_log[-3000:]
  time.sleep(.1)
 records['duplicate_id_rejection_log']=rejected_log
 rejected=server.apps(s);records['duplicate_id_retained']=rejected;save()
 assert rejected['catalog_generation']==probes['catalog_generation']
 assert [(x['app_id'],x['title']) for x in rejected['apps']]==[(x['app_id'],x['title']) for x in probes['apps']]
 # Restore a valid candidate, open it while executable exists, then remove
 # the executable without notifying refresh. Worker must revalidate on click.
 job('/boot/BIN/busybox cp "$1" "$2"; printf "%s" "$3" >> "$2"',backup,manifest,rows,uid=0)
 live=open_menu();records['before_removed_executable']=live;save()
 index=next(i for i,x in enumerate(live['apps']) if x['desktop_app_id']=='probe.one')
 job('/boot/BIN/busybox rm "$1"',probe,uid=0)
 data=server.load_session(s);serial=Path(data['serial_log']);offset=serial.stat().st_size
 records['removed_executable_click']=click(*item_xy(live,index));menu_open=False
 end=time.monotonic()+10
 while True:
  text=serial.read_bytes()[offset:].decode(errors='replace')
  if 'dispatch failed result=-2' in text:break
  assert time.monotonic()<end,text[-3000:]
  time.sleep(.1)
 records['removed_executable_log']=text;save()
 print('EQUAL TITLES / DUPLICATE-ID REJECTION / STALE EXECUTABLE DISPATCH REJECTION PASS',flush=True)
finally:
 job('/boot/BIN/busybox cp "$1" "$2"; /boot/BIN/busybox rm -f "$3"',backup,manifest,probe,uid=0)
 restored=open_menu();records['restored']=restored;save()
 assert [(x['app_id'],x['title']) for x in restored['apps']]==[(x['app_id'],x['title']) for x in base['apps']]
 f=server.observe(s);(evidence/'restored-applications-menu.png').write_bytes(f['image'])
 click(20,1058);menu_open=False
print('ACCEPTED; original catalog restored',flush=True)
