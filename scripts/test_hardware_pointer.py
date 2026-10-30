#!/usr/bin/env python3
"""Private QMP test: desktop DPI gain, raw capture, escape release and cursor image.

Only accepts a private localhost test-harness ready file. No physical/host input.
"""
import argparse,json,re,socket,sys,time,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'tools/vm_bridge')]
from hardware import Hardware
from hardware_bulk_workflow import BulkRun
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ready',type=Path,required=True);p.add_argument('--key-file',type=Path,required=True);p.add_argument('--probe',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
r=json.loads(a.ready.read_text());h=Hardware(r['registry']);session=r['session'];assert h.load_session(session)['host']=='127.0.0.1'
o=a.output.resolve();o.mkdir(parents=True,exist_ok=False);result={'scope':'private QMP pointer DPI/capture'}
def save():(o/'result.json').write_text(json.dumps(result,indent=2)+'\n')
def wait(i,condition=lambda j:j.get('state')=='completed'):
    end=time.monotonic()+30
    while time.monotonic()<end:
        j=h.job(session,i,wait_s=1)
        if condition(j):return j
        assert j.get('state') not in ('failed','lost','rejected','completed'),j
    raise TimeoutError('Inspect original job '+i)
def run(label,argv,uid=1000):
    i=uuid.uuid4().hex;result[label+'_job']=i;save();h.guest_run(session,i,argv,cwd='/tmp',uid=uid,wait_s=2);j=wait(i);assert j.get('exit',{}).get('code')==0,j;return j
sock=socket.socket(socket.AF_UNIX);sock.settimeout(10);sock.connect(r['qmp']);f=sock.makefile('rwb',buffering=0);f.readline()
def qmp(command,arguments=None):
    f.write((json.dumps({'execute':command,**({'arguments':arguments} if arguments else {})})+'\n').encode())
    while True:
        reply=json.loads(f.readline())
        if 'return' in reply:return reply['return']
        if 'error' in reply:raise RuntimeError(reply)
def send(events):qmp('input-send-event',{'events':events});time.sleep(.15)
def rel(x,y):send([{'type':'rel','data':{'axis':axis,'value':value}} for axis,value in [('x',x),('y',y)] if value])
def key(code,down):return {'type':'key','data':{'down':down,'key':{'type':'qcode','data':code}}}
def rows(j):return [(int(t),int(x),int(y),int(token)) for t,x,y,token in re.findall(r'POINTER type=(\d+) x=(-?\d+) y=(-?\d+) token=(\d+)',j.get('stdout',{}).get('text',''))]
try:
    qmp('qmp_capabilities');b=BulkRun(h,o/'upload').execute(session,a.key_file,'put',source=a.probe,port=r['bulk_port']);result['upload']=b;save()
    run('chmod',['/nix/profile/bin/chmod','755',b['transfer']['guest_path']],0)
    job=uuid.uuid4().hex;result['probe_job']=job;save();h.guest_run(session,job,[b['transfer']['guest_path'],'200'],cwd='/tmp',uid=1000,wait_s=2)
    initial=wait(job,lambda j:'POINTER_READY scale=200' in j.get('stdout',{}).get('text',''));result['initial']=initial;save()
    # A fresh private PS/2 pointer starts at0,0. Move inside the probe client.
    rel(100,100);before=wait(job,lambda j:any(t==2 for t,x,y,k in rows(j)));old=[e for e in rows(before) if e[0]==2][-1]
    count=len(rows(before));rel(10,0);after=wait(job,lambda j:any(t==2 and x==old[1]+20 for t,x,y,k in rows(j)[count:]) and (6,10,0,0) in rows(j)[count:]);new=[e for e in rows(after) if e[0]==2][-1]
    assert new[1]-old[1]==20,(old,new);assert (6,10,0,0) in rows(after),rows(after)
    result['desktop_pixels_per_10_counts']=new[1]-old[1];result['desktop_events']=rows(after);save()
    qmp('screendump',{'filename':str(o/'desktop-cursor.ppm')})
    send([key('c',True)]);send([key('c',False)])
    captured=wait(job,lambda j:bool(re.search(r'CAPTURE token=[1-9]\d*',j.get('stdout',{}).get('text',''))));token=int(re.findall(r'CAPTURE token=(\d+)',captured['stdout']['text'])[-1]);assert token>0
    rel(10,0);raw=wait(job,lambda j:(6,10,0,token) in rows(j));result['capture_token']=token;result['captured_raw_events']=rows(raw);save()
    send([key('ctrl',True),key('alt',True),key('esc',True)]);send([key('esc',False),key('alt',False),key('ctrl',False)])
    count=len(rows(raw));rel(10,0);released=wait(job,lambda j:len(rows(j))>count and rows(j)[-1][3]==0);assert any(t==2 for t,x,y,k in rows(released)[count:]),rows(released)
    result['release_observation']=released;save()
    w=[w for w in h.windows(session)['windows'] if w.get('provider',{}).get('pid')==initial['guest']['pid']];assert len(w)==1
    close=uuid.uuid4().hex;result['close_request']=close;save();h.window(session,w[0]['reference'],'close',close,wait_for='client_group_exit',wait_s=2)
    result['exit']=wait(job);assert result['exit'].get('exit',{}).get('code')==0
    result['passed']=True;save();print(json.dumps({'passed':True,'evidence':str(o/'result.json'),'cursor_image':str(o/'desktop-cursor.ppm')},indent=2))
except Exception as e:result['passed']=False;result['error']=str(e);save();raise
finally:f.close();sock.close()
