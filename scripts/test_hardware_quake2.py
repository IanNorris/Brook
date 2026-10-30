#!/usr/bin/env python3
"""Stage a private Quake2 PAK over binary TCP and test CPU-rendered animation.

Game assets are caller-supplied, never copied into Git or the public image.
The fixed install is ephemeral /data/home/quake2. Run on private QEMU first.
"""
import argparse,hashlib,json,struct,sys,time,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'tools/vm_bridge')]
from hardware import Hardware
from hardware_bulk_workflow import BulkRun
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--ready',type=Path);p.add_argument('--session');p.add_argument('--registry',type=Path)
p.add_argument('--port',type=int,default=1236);p.add_argument('--key-file',type=Path,required=True)
p.add_argument('--engine',type=Path,required=True);p.add_argument('--pak0',type=Path,required=True)
p.add_argument('--output',type=Path,required=True);p.add_argument('--leave-running',action='store_true')
a=p.parse_args();ready=json.loads(a.ready.read_text()) if a.ready else None
if not ready and not a.session:p.error('Use --ready or --session')
h=Hardware(ready['registry'] if ready else a.registry);session=ready['session'] if ready else a.session;port=ready['bulk_port'] if ready else a.port
data=h.load_session(session)
if ready:assert data['host']=='127.0.0.1','Private QEMU ready file required'
with a.engine.open('rb') as f:assert f.read(4)==b'\x7fELF','Engine must be ELF'
with a.pak0.open('rb') as f:magic,offset,length=struct.unpack('<4sII',f.read(12))
assert magic==b'PACK' and length%64==0 and offset+length<=a.pak0.stat().st_size,'Invalid PAK header'
out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);home='/data/home/quake2';results={'session':session,'boot_id':data['guest_boot_id'],'renderer':'native framebuffer ref_soft','audio':'disabled; Dell SST codec is not implemented'}
def save(): (out/'result.json').write_text(json.dumps(results,indent=2)+'\n')
def wait(ident,seconds=90):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        r=h.job(session,ident,wait_s=2)
        if r.get('state')=='completed':return r
        assert r.get('state') not in ('failed','rejected','lost'),r
        time.sleep(.05)
    raise TimeoutError('Inspect original job '+ident)
def run(label,argv,uid=0):
    ident=uuid.uuid4().hex;results[label+'_job']=ident;save()
    h.guest_run(session,ident,argv,cwd='/tmp',uid=uid,wait_s=2);r=wait(ident)
    results[label]=r;save();assert r.get('exit',{}).get('code')==0,r
    return r
def transfer(label,action,**kwargs):
    r=BulkRun(h,out/label).execute(session,a.key_file,action,port=port,**kwargs);results[label]=r;save();return r
def stop():
    if results.get('game_pid'):
        run('stop',['/nix/profile/bin/bash','-c','kill -TERM "$1"','brook-stop',str(results['game_pid'])])
        results['game_exit']=wait(results['game_job']);save()
def crop_bytes(raw,w,x,y,cw,ch):return b''.join(raw[((y+i)*w+x)*4:((y+i)*w+x+cw)*4] for i in range(ch))
try:
    results['doctor']=h.doctor(session);save()
    # Never overwrite a pre-existing farm/config/game directory silently.
    run('install-preflight',['/nix/profile/bin/bash','-c','test ! -e "$1"','brook-test',home])
    run('mkdir',['/nix/profile/bin/mkdir','-p',home+'/baseq2'])
    engine=transfer('engine-upload','put',source=a.engine)
    run('install-engine',['/nix/profile/bin/mv','--',engine['transfer']['guest_path'],home+'/quake2'])
    run('chmod-engine',['/nix/profile/bin/chmod','755',home+'/quake2'])
    pak=transfer('pak-upload','put',source=a.pak0)
    run('install-pak',['/nix/profile/bin/mv','--',pak['transfer']['guest_path'],home+'/baseq2/pak0.pak'])
    run('chmod-pak',['/nix/profile/bin/chmod','444',home+'/baseq2/pak0.pak'])
    run('home-owner',['/nix/profile/bin/chown','1000:1000',home,home+'/baseq2'])
    game=uuid.uuid4().hex;results['game_job']=game;save()
    argv=[home+'/quake2','+set','basedir',home,'+set','vid_ref','soft','+set','sw_mode','3',
        '+set','s_initsound','0','+set','cd_nocd','1','+set','timedemo','0','+demomap','demo1.dm2']
    started=time.monotonic();h.guest_run(session,game,argv,cwd=home,uid=1000,wait_s=2)
    end=time.monotonic()+90;window=None
    while time.monotonic()<end:
        job=h.job(session,game,wait_s=2);results['game_observation']=job;save()
        assert job.get('state') not in ('completed','failed','rejected','lost'),job
        pid=job.get('guest',{}).get('pid',0)
        windows=h.windows(session)
        matches=[w for w in windows['windows'] if w.get('provider',{}).get('pid')==pid and w.get('visible')]
        if pid and len(matches)==1:window=matches[0];results['game_pid']=pid;break
        time.sleep(.2)
    assert window,'Game window not observed'
    results['window']=window;results['window_observed_s']=time.monotonic()-started;save()
    # Test the scene interior, excluding frame counter/HUD/taskbar changes.
    captures=[]
    for label in ['capture-a','capture-b']:
        time.sleep(2)
        r=transfer(label,'frame',keep_raw=True);raw=Path(r['transfer']['raw_artifact_path']).read_bytes()
        w=r['frame']['width'];height=r['frame']['height'];cx=window['x']+window['outer_width']//2;cy=window['y']+window['outer_height']//2
        x=max(0,min(cx-96,w-192));y=max(0,min(cy-64,height-128))
        patch=crop_bytes(raw,w,x,y,192,128);captures.append(patch)
        results[label+'_scene_sha256']=hashlib.sha256(patch).hexdigest();Path(r['transfer']['raw_artifact_path']).unlink();save()
    changed=sum(a!=b for a,b in zip(captures[0],captures[1]));results['scene_changed_bytes']=changed
    assert changed>1000 and len(set(captures[1]))>32,'Scene animation not established; inspect saved screenshots and game job'
    results['stats']=h.stats(session);results['game_observation']=h.job(session,game,wait_s=2)
    assert results['game_observation'].get('state') not in ('completed','failed','rejected','lost')
    results['passed']=True;save()
    if not a.leave_running:stop()
    if ready and not a.leave_running:
        (a.ready.parent/'finish.json').write_text(json.dumps({'passed':True,'scope':'Quake2 software scene animation using private caller PAK','evidence':str(out/'result.json')},indent=2)+'\n')
    print(json.dumps({'passed':True,'game_pid':results['game_pid'],'window_observed_s':results['window_observed_s'],'changed_scene_bytes':changed,'screenshot':results['capture-b']['artifact_path'],'left_running':a.leave_running},indent=2))
except Exception as error:
    results['passed']=False;results['error']=str(error);save()
    if ready:
        try:stop()
        except Exception as cleanup:results['cleanup_error']=str(cleanup);save()
        (a.ready.parent/'finish.json').write_text(json.dumps({'passed':False,'error':str(error),'evidence':str(out/'result.json')},indent=2)+'\n')
    raise
