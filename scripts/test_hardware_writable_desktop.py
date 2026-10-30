#!/usr/bin/env python3
"""Private USB-QEMU writable desktop, mmap/COW and restart-persistence gate.

Creates one new regular-file USB disk. No physical media, host desktop, GPU or
speakers. A reviewer owns only this headless VM while ready.json is published;
finish.json with continue_storage_checks=true returns control for checked
shutdown and a fresh-process boot. UI acceptance is reported separately; a
successful storage test does not certify browser or Gallery functionality.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time
import uuid
from test_hardware_power import Qmp, Rpc
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'tools/vm_bridge')]
from hardware import Hardware


def run(*argv,**kwargs):
    return subprocess.run(list(map(str,argv)),check=True,cwd=ROOT,**kwargs)


def sha(path):
    with path.open('rb') as source:return hashlib.file_digest(source,'sha256').hexdigest()


def save(path,data):path.write_text(json.dumps(data,indent=2)+'\n')


def wait_job(h,s,ident,timeout=120):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        job=h.job(s,ident,wait_s=2)
        if job['state']=='completed':return job
        assert job['state'] not in ('failed','rejected','lost'),job
    raise TimeoutError('Inspect same job '+ident)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--desktop',required=True,type=Path)
    p.add_argument('--recovery',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('--disk-template',type=Path,help='Private stopped regular-file disk to clone; caller must first observe its QEMU owner exited')
    p.add_argument('--review-seconds',type=int,default=1200)
    a=p.parse_args();desktop=a.desktop.resolve(strict=True);recovery=a.recovery.resolve(strict=True)
    out=a.output.absolute();out.mkdir(parents=True,exist_ok=False)
    meta=json.loads((desktop/'manifest.json').read_text());store=Path(meta['writable_desktop']['store_artifact'])
    image=store/'STORE.IMG';assert sha(image)==meta['writable_desktop']['initial_sha256']
    for name in ('kernel.elf','INITRD.IMG'):assert sha(desktop/name)==meta['files'][name]['sha256']
    disk=out/'private.raw';partition_bytes=6*1024**3
    if a.disk_template:
        template=a.disk_template.absolute();assert template.is_file() and not template.is_symlink()
        assert template.stat().st_size==partition_bytes+2*1024**2
        run('cp','--reflink=auto','--',template,disk)
    else:
        with disk.open('xb') as stream:stream.truncate(partition_bytes+2*1024**2)
        layout=f'label: dos\nunit: sectors\n\nstart=2048,size={partition_bytes//512},type=c,bootable\n'
        run('sfdisk',disk,input=layout,text=True,stdout=subprocess.DEVNULL)
        run('mkfs.fat','-F','32','--offset','2048',disk,str(partition_bytes//1024))
    media=str(disk)+'@@1048576'
    payload=recovery/'payload'
    if not a.disk_template:
        for name in ('EFI','KERNEL','INITRD.IMG','BROOK.CFG','BROOK.MNT','APPS.BRK'):
            run('mcopy','-s','-i',media,payload/name,'::/')
    run('mcopy','-o','-i',media,desktop/'kernel.elf','::KERNEL/BROOK.ELF')
    run('mcopy','-o','-i',media,desktop/'INITRD.IMG','::INITRD.IMG')
    cfg=out/'BROOK.CFG';cfg.write_text('TARGET=KERNEL\\BROOK.ELF\nDEBUG_TEXT=1\nDISPLAY_WIDTH=1920\nDISPLAY_HEIGHT=1080\n')
    run('mcopy','-o','-i',media,cfg,'::BROOK.CFG')
    if not a.disk_template:
        run('mcopy','-i',media,image,'::STORE.IMG')
        run('mcopy','-i',media,desktop/'writable-desktop','::STORE.OK')
    result={'scope':'private USB-QEMU writable desktop','phases':[]};token=uuid.uuid4().hex
    save(out/'result.json',result)
    for phase in ('create','verify'):
        base=out/phase;base.mkdir();variables=base/'VARS.fd';shutil.copy2(os.environ['OVMF_VARS'],variables);variables.chmod(0o600)
        with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
        argv=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','8','-m','8192','-nodefaults',
            '-drive','if=pflash,format=raw,readonly=on,file='+os.environ['OVMF_CODE'],
            '-drive','if=pflash,format=raw,file='+str(variables),'-device','VGA,vgamem_mb=64',
            '-device','qemu-xhci,id=usbc','-drive','if=none,id=stick,format=raw,file='+str(disk),
            '-device','usb-storage,bus=usbc.0,port=1,drive=stick,bootindex=1',
            '-device','virtio-tablet-pci',
            '-device','usb-net,bus=usbc.0,port=2,netdev=network',
            '-netdev',f'user,id=network,hostfwd=tcp:127.0.0.1:{port}-:1234',
            '-display','none','-serial','file:'+str(base/'serial.log'),
            '-qmp','unix:'+str(base/'qmp.sock')+',server=on,wait=off',
            '-qmp','unix:'+str(base/'qmp-review.sock')+',server=on,wait=off','-no-reboot']
        save(base/'argv.json',argv);log=(base/'qemu.log').open('x');owner=subprocess.Popen(argv,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT);q=None;rpc=None
        try:
            end=time.monotonic()+300
            while time.monotonic()<end:
                assert owner.poll() is None,'QEMU exited before desktop boot'
                serial=(base/'serial.log').read_text(errors='replace') if (base/'serial.log').exists() else ''
                assert 'PANIC_HEX:' not in serial,serial[-3000:]
                if 'HARDWARE_NETWORK_READY' in serial and 'DEBUG_TCP: listening' in serial:break
                time.sleep(.2)
            else:raise TimeoutError(serial[-4000:])
            q=Qmp(base/'qmp.sock');rpc=Rpc(base);rpc.request('initialize',{'protocolVersion':'2025-06-18'})
            attached=rpc.tool('hw_attach',{'host':'127.0.0.1','port':port});s=attached['session'];h=Hardware(base/'registry')
            doctor=h.doctor(s);save(base/'doctor.json',doctor)
            assert 'writable desktop: ready' in serial and 'immutable RAM application snapshot ready' not in serial,doctor
            assert doctor['guest']['writable_desktop']['ready'] and not doctor['guest']['app_snapshot']['configured'],doctor
            ident=uuid.uuid4().hex;save(base/'storage-job-request.json',{'session':s,'request_id':ident,'operation':phase,'token':token})
            h.guest_run(s,ident,['/boot/STOREQA.ELF',phase,token],cwd='/tmp',uid=1000,wait_s=2)
            job=wait_job(h,s,ident);save(base/'storage-job.json',job)
            assert job.get('exit',{}).get('code')==0 and 'WRITABLE_DESKTOP_' in job['stdout']['text'],job
            if phase=='create':
                ready={**attached,'registry':str(base/'registry'),'qmp':str(base/'qmp-review.sock'),
                    'qemu_pid':owner.pid,'scope':'Private headless QEMU only; 8CPU/8GiB software graphics; user networking',
                    'instruction':'Review Firefox/Chromium/App Gallery; close launched apps and return via finish.json continue_storage_checks=true. Record ui_acceptance_passed separately with evidence and remaining failures. Physical host desktop and real Dell are out of scope.'}
                save(out/'ready.json',ready);print('WRITABLE_DESKTOP_QEMU_READY',out/'ready.json',flush=True)
                end=time.monotonic()+a.review_seconds
                while not (out/'finish.json').exists():
                    assert owner.poll() is None,'QEMU exited during review'
                    assert time.monotonic()<end,'Review expired; preserve failure disk and inspect original VM'
                    time.sleep(.2)
                reviewed=json.loads((out/'finish.json').read_text());assert reviewed.get('continue_storage_checks') is True,reviewed
                result['review']=reviewed
            shutdown=uuid.uuid4().hex;state=h.power(s,shutdown,'shutdown');end=time.monotonic()+120
            while state['state']!='ready':
                assert state['state']!='failed' and time.monotonic()<end,state
                time.sleep(.2);state=h.power(s,shutdown,'status')
            save(base/'shutdown.json',state)
            clean=h.command(s,'bridge hardware caps')['writable_desktop'];save(base/'clean.json',clean)
            assert clean['clean_finalized'] and not clean['dirty_session'] and not clean['state_uncertain'] and not clean['final_error'],clean
            q.call('quit');assert owner.wait(timeout=15)==0
            result['phases'].append({'operation':phase,'mmap_cow_persistence':True,'checked_shutdown':True,'clean_finalized':True});save(out/'result.json',result)
        finally:
            if rpc:rpc.close()
            if q:q.close()
            if owner.poll() is None:owner.terminate();owner.wait(timeout=15)
            log.close()
    recovered=out/'recovered-store.img';run('mcopy','-i',media,'::STORE.IMG',recovered)
    with (out/'e2fsck.log').open('x') as log:run('e2fsck','-fn',recovered,stdout=log,stderr=subprocess.STDOUT)
    with recovered.open('rb') as f:f.seek(1082);assert f.read(2)==b'\1\0'
    recovered.unlink();result['passed']=True;result['storage_acceptance_passed']=True
    result['ui_acceptance_passed']=result['review'].get('ui_acceptance_passed') is True
    result['standard_ext2_check']=True;save(out/'result.json',result)
    print('WRITABLE_DESKTOP_QEMU_PASS',flush=True)


if __name__=='__main__':main()
