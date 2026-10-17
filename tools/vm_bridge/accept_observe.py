#!/usr/bin/env python3
"""Selectable observation acceptance on an empty disposable 1920x1080 VM."""
import argparse,json,time,uuid
from pathlib import Path
import server
from media import Delivery

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--session',required=True);p.add_argument('--out',required=True,type=Path)
a=p.parse_args();s=a.session;out=a.out.resolve()
if not out.is_relative_to((server.ROOT/'build').resolve()):raise ValueError('Keep acceptance evidence under build/')
out.mkdir(parents=True,exist_ok=True);evidence={}
def save(): (out/'observe.json').write_text(json.dumps(evidence,indent=2))
def observe(name,include,**kw):
 original=server.guest;commands=[]
 def traced(data,command,timeout=8):
  commands.append(command);return original(data,command,timeout=timeout)
 server.guest=traced;begin=time.monotonic()
 try:r=server.observe(s,include=include,**kw)
 finally:server.guest=original
 delivery=Delivery(server.REGISTRY,s);adapted=delivery.adapt(r)
 evidence[name]={'result':adapted,'commands':commands,'elapsed_s':time.monotonic()-begin};save()
 assert r['state']=='completed',(name,adapted)
 assert r['identity_verified_at_end'] and not r['consistency']['atomic']
 assert r['consistency']['frame_window_correspondence']=='unverified'
 print(name,round(time.monotonic()-begin,3),'seconds',len(commands),'guest exchanges',flush=True)
 return r
if server.windows(s)['windows']:raise ValueError('Start with an empty disposable desktop')
apps=server.apps(s);targets=[]
for title in ['Terminal','Mousepad','Settings']:
 app=next(x for x in apps['apps'] if x['title']==title)
 r=server.launch_app(s,app['reference'],uuid.uuid4().hex,wait_s=15)
 if not r['wait_condition_met']:r=server.ui_status(s,r['operation_id'],wait_for='window',wait_s=15)
 evidence.setdefault('launches',[]).append(r);save();assert r['wait_condition_met'],r
 targets.append(r['current_window']['reference'])
r=observe('metadata_only',['windows','processes','launches','errors'])
assert sum(x.startswith('bridge ui observe ') for x in evidence['metadata_only']['commands'])==1
assert not any('bridge frame ' in x for x in evidence['metadata_only']['commands'])
tasks=r['components']['processes']['result']['tasks'];windows=r['components']['windows']['result']['windows']
assert not r['components']['processes']['result']['truncated']
for ref in targets:
 w=next(x for x in windows if x['reference']==ref)
 client=w['client'];task=next(x for x in tasks if x['pid']==client['pid'] and x['incarnation']==client['incarnation'])
 assert task['euid']==1000 and task['uid']==1000,(w,task)
 assert task['reference']==client['reference']
launches=r['components']['launches']['result']['launches']
for launch in evidence['launches']:
 assert any(x['token']==launch['launch']['token'] for x in launches)
full=observe('full',['frame','windows','processes','launches','errors'])
f=full['components']['frame']['result'];(out/'observed-desktop.png').write_bytes(f['image'])
assert f['data']['guest_boot_id']==full['guest_boot_id'] and f['data']['geometry_provenance']['verified_for_current_boot']
assert (f['data']['width'],f['data']['height'])==(1920,1080)
# Saved views exercise the actual MCP adapter while proving that follow-up
# previews/crops/coordinate rulers make no guest or QMP call.
source=f['data'];original_guest=server.guest;original_qmp=server.QMP
try:
 def no_vm(*args,**kwargs):raise AssertionError('Saved views must not contact the VM')
 server.guest=no_vm;server.QMP=no_vm
 for name,options in [('tiny',{'size':'tiny'}),('medium',{'size':'medium'}),
                      ('ruler',{'size':'medium','overlay':True}),
                      ('native_crop',{'crop':{'x':100,'y':80,'width':820,'height':620}})]:
  wire=server.call_tool('vm_frame',{'session':s,'frame_id':source['frame_id'],**options})
  assert not wire['isError'] and all(x['type']=='text' for x in wire['content'])
  view=json.loads(wire['content'][0]['text'])
  assert view['source_frame_id']==source['frame_id'] and view['frame_epoch']==source['frame_epoch']
  assert not view['vm_capture_performed']
  if name=='native_crop':assert (view['width'],view['height'])==(820,620)
  if name=='ruler':assert view['overlay']['coordinate_space']=='native_frame'
  evidence.setdefault('saved_views',{})[name]=view;save()
finally:server.guest=original_guest;server.QMP=original_qmp
cursor=full['components']['errors']['result']['next_cursor']
next_read=observe('incremental',['errors'],since_event_cursor=cursor)
assert int(next_read['components']['errors']['result']['next_cursor']['offset'])>=int(cursor['offset'])
crop=observe('crop',['frame'],crop={'x':100,'y':80,'width':1000,'height':700},max_width=800)
f=crop['components']['frame']['result']['data'];assert (f['width'],f['height'])==(800,560)
assert f['crop_origin']==[100,80] and f['coordinate_width']==1920
# Use the actual shared MCP result adapter too; inspect text metadata, never
# serialize an image-bearing preview envelope.
wire=server.call_tool('vm_observe',{'session':s,'include':['frame','windows']})
assert not wire['isError'] and all(x['type']=='text' for x in wire['content'])
text=wire['content'][0]['text'];safe=json.loads(text);assert 'base64' not in text and 'data:image' not in text
evidence['mcp_default']=safe;save()
for row in server.windows(s)['windows']:
 r=server.window_action(s,row['reference'],'close',uuid.uuid4().hex,wait_for='closed',wait_s=10)
 evidence.setdefault('closes',[]).append(r);save();assert r['wait_condition_met'],r
print('ACCEPTED selected snapshots / desktop identity / task and window references / guarded frames / cursor / crop / reference-only MCP',flush=True)
