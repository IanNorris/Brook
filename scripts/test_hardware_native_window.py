#!/usr/bin/env python3
"""Private QEMU native-self origin, relay isolation and cooperative-close test."""
import argparse,json,sys,time,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'tools/vm_bridge')]
from hardware import Hardware
from hardware_bulk_workflow import BulkRun
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ready',type=Path,required=True);p.add_argument('--key-file',type=Path,required=True);p.add_argument('--probe',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
r=json.loads(a.ready.read_text());h=Hardware(r['registry']);s=r['session'];assert h.load_session(s)['host']=='127.0.0.1'
o=a.output.resolve();o.mkdir(parents=True,exist_ok=False);result={'scope':'Private guest native self-origin and cooperative close'}
def save():(o/'result.json').write_text(json.dumps(result,indent=2)+'\n')
def wait(i):
    end=time.monotonic()+45
    while time.monotonic()<end:
        j=h.job(s,i,wait_s=2)
        if j.get('state')=='completed':return j
        assert j.get('state') not in ('failed','rejected','lost'),j
    raise TimeoutError('Inspect original '+i)
def run(label,argv,complete=True):
    i=uuid.uuid4().hex;result[label+'_job']=i;save();j=h.guest_run(s,i,argv,cwd='/tmp',uid=1000,wait_s=2)
    if complete:j=wait(i);assert j.get('exit',{}).get('code')==0,j
    result[label]=j;save();return i
try:
    b=BulkRun(h,o/'upload').execute(s,a.key_file,'put',source=a.probe,port=r['bulk_port']);result['upload']=b;save()
    # Root applies executable permissions; callers remain ordinary UID1000.
    i=uuid.uuid4().hex;result['chmod_job']=i;save();h.guest_run(s,i,['/nix/profile/bin/chmod','755',b['transfer']['guest_path']],cwd='/tmp',uid=0,wait_s=2);assert wait(i).get('exit',{}).get('code')==0
    for label,argv in [('relay',[b['transfer']['guest_path']]),('native',[b['transfer']['guest_path'],'--native'])]:
        i=run(label,argv,False);end=time.monotonic()+30
        while time.monotonic()<end:
            j=h.job(s,i,wait_s=2);pid=j.get('guest',{}).get('pid');windows=[w for w in h.windows(s)['windows'] if w.get('provider',{}).get('pid')==pid]
            if len(windows)==1:break
            assert j.get('state')!='completed',j
        else:raise TimeoutError('Window missing')
        w=windows[0];result[label+'_window']=w;save()
        if label=='relay':
            assert w['client_origin']=='unknown' and w['client'] is None and 'close' not in w['supports'],w
            run('stop-relay',['/nix/profile/bin/bash','-c','kill -TERM "$1"','native-test',str(pid)])
            result['relay_exit']=wait(i);save()
        else:
            assert w['client_origin']=='native_process' and w['client']['pid']==pid and 'close' in w['supports'],w
            close=uuid.uuid4().hex;result['close_request']=close;save()
            c=h.window(s,w['reference'],'close',close,wait_for='client_group_exit',wait_s=2)
            end=time.monotonic()+30
            while c.get('state') not in ('completed','failed','rejected') and time.monotonic()<end:c=h.ui_status(s,close,wait_for='client_group_exit',wait_s=2)
            result['close']=c;result['native_exit']=wait(i);save()
            assert result['native_exit'].get('exit',{}).get('code')==0,result['native_exit']
            assert 'NATIVE_WINDOW_CLOSED' in result['native_exit'].get('stdout',{}).get('text','')
    result['passed']=True;save();print(json.dumps({'passed':True,'evidence':str(o/'result.json')},indent=2))
except Exception as e:result['passed']=False;result['error']=str(e);save();raise
