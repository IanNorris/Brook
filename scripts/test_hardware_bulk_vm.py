#!/usr/bin/env python3
"""Real private-QEMU bulk helper and completed-frame syscall acceptance."""
import argparse,hashlib,json,re,socket,sys,time,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'tools/vm_bridge')]
from hardware import Hardware
from hardware_bulk_workflow import BulkRun
from hardware_bulk_protocol import authenticate,line
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ready',type=Path,required=True);p.add_argument('--key-file',type=Path,required=True);p.add_argument('--probe',type=Path,required=True)
a=p.parse_args();ready_path=a.ready.resolve(strict=True);ready=json.loads(ready_path.read_text());out=ready_path.parent/'bulk-qa';out.mkdir(exist_ok=False)
h=Hardware(ready['registry']);session=ready['session'];data=h.load_session(session)
assert data['host']=='127.0.0.1' and ready['qemu_pid']>0,'Private QEMU only'
results={}
def save(): (out/'result.json').write_text(json.dumps(results,indent=2)+'\n')
def job(argv,wait=True):
    ident=uuid.uuid4().hex;h.guest_run(session,ident,argv,cwd='/tmp',uid=0,wait_s=2)
    return wait_job(ident) if wait else ident
def wait_job(ident,contains=None,seconds=70):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        r=h.job(session,ident,wait_s=2)
        if contains and contains in r.get('stdout',{}).get('text',''):return r
        if r.get('state')=='completed':
            assert not contains and r.get('exit',{}).get('code')==0,r
            return r
        assert r.get('state') not in ('failed','rejected','lost'),r
        time.sleep(.05)
    raise TimeoutError(ident)
def transfer(name,action,**kwargs):return BulkRun(h,out/name).execute(session,a.key_file,action,port=ready['bulk_port'],**kwargs)
try:
    results['doctor']=h.doctor(session);assert results['doctor']['capabilities']['frame_capture'] is True
    payload=out/'payload.bin';payload.write_bytes(bytes(range(251))*8193+b'private-qemu-end')
    uploaded=transfer('upload','put',source=payload);artifact=uploaded['transaction_id'];results['upload']=uploaded
    downloaded=transfer('download','get',artifact_id=artifact);results['download']=downloaded
    assert Path(downloaded['artifact_path']).read_bytes()==payload.read_bytes();save()
    probe=transfer('probe-upload','put',source=a.probe);path=probe['transfer']['guest_path']
    results['chmod']=job(['/nix/profile/bin/chmod','755',path])
    results['probe']=job([path]);assert 'FRAME_PROBE_PASS' in results['probe']['stdout']['text'];save()
    frame=transfer('screenshot','frame',keep_raw=True);results['screenshot']=frame
    mode=re.search(r'^Framebuffer\s+(\d+)x(\d+)',(ready_path.parent/'serial.log').read_text(errors='replace'),re.M);assert mode
    assert (frame['frame']['width'],frame['frame']['height'])==(int(mode[1]),int(mode[2]))
    assert Path(frame['artifact_path']).read_bytes().startswith(b'\x89PNG\r\n\x1a\n')
    raw=Path(frame['transfer'].get('raw_artifact_path') or out/'screenshot/artifact.bin').read_bytes();assert len(raw)==frame['frame']['width']*frame['frame']['height']*4
    assert len(set(raw))>16;save()
    # Pause a real download after its immutable payload header. Another task
    # must still acquire/copy/release a new frame while the network is blocked.
    slow=uuid.uuid4().hex;h.guest_run(session,slow,['/boot/HWBULK.ELF',data['guest_boot_id'],slow,'frame'],cwd='/tmp',uid=0,wait_s=2)
    wait_job(slow,'HWBULK_LISTENING')
    with socket.create_connection(('127.0.0.1',ready['bulk_port']),timeout=30) as connection:
        connection.settimeout(60);authenticate(connection,a.key_file.read_text(),data['guest_boot_id'],slow);meta=line(connection)
        assert meta['state']=='payload' and meta['bytes']==len(raw)
        results['probe_during_paused_download']=job([path]);assert 'FRAME_PROBE_PASS' in results['probe_during_paused_download']['stdout']['text']
        left=meta['bytes'];digest=hashlib.sha256()
        while left:
            chunk=connection.recv(min(65536,left));assert chunk;digest.update(chunk);left-=len(chunk)
        assert digest.hexdigest()==meta['sha256'];assert line(connection)['state']=='completed'
    results['paused_download']=wait_job(slow);save()
    held=job([path,'--hold'],False);results['hold']=wait_job(held,'FRAME_PROBE_HOLD')
    try:transfer('capture-contention','frame')
    except RuntimeError as error:results['contention_error']=str(error)
    else:raise AssertionError('Capture stole an existing lease')
    results['hold_completed']=wait_job(held);save()
    results['expiry']=job([path,'--expiry']);assert 'FRAME_PROBE_EXPIRY_PASS' in results['expiry']['stdout']['text'];save()
    frame2=transfer('screenshot-after-expiry','frame');results['screenshot_after_expiry']=frame2
    assert frame2['frame']['frame_epoch']>frame['frame']['frame_epoch']
    results['passed']=True;save()
    (ready_path.parent/'finish.json').write_text(json.dumps({'passed':True,'scope':'Real binary upload/download, root-only owner-bound completed capture, immutable chunk, range/EFAULT/stale checks, contention, expiry, hash-verified PNG paths','evidence':'bulk-qa/result.json'},indent=2)+'\n')
    print('HARDWARE_BULK_QEMU_PASS',frame2['artifact_path'])
except Exception as error:
    results['passed']=False;results['error']=str(error);save()
    (ready_path.parent/'finish.json').write_text(json.dumps({'passed':False,'error':str(error),'evidence':'bulk-qa/result.json'},indent=2)+'\n')
    raise
