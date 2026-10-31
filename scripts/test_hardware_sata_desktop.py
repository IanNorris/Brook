#!/usr/bin/env python3
"""Private QEMU SATA-only writable desktop, checked reboot and ext2 gate.

Creates a new regular-file GPT disk in the output directory. No physical disk
or USB storage is passed to QEMU. The source store is copied, never modified.
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

ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'tools/vm_bridge')]
from hardware import Hardware
from test_hardware_power import Qmp


def run(*args,**kwargs):return subprocess.run([str(x) for x in args],cwd=ROOT,check=True,**kwargs)
def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def save(path,data):path.write_text(json.dumps(data,indent=2)+'\n')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--desktop',required=True,type=Path);p.add_argument('--recovery',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path)
    a=p.parse_args();desktop=a.desktop.resolve(strict=True);recovery=a.recovery.resolve(strict=True)
    out=a.output.absolute();out.mkdir(parents=True,exist_ok=False)
    meta=json.loads((desktop/'manifest.json').read_text());store=Path(meta['writable_desktop']['store_artifact'])/'STORE.IMG'
    assert sha(store)==meta['writable_desktop']['initial_sha256']
    for name in ('kernel.elf','INITRD.IMG'):assert sha(desktop/name)==meta['files'][name]['sha256']
    disk=out/'sata.raw';disk_bytes=6*1024**3+2*1024**2;sectors=disk_bytes//512
    first=2048;count=sectors-4096;disk_guid=uuid.uuid4();part_guid=uuid.uuid4()
    with disk.open('xb') as f:f.truncate(disk_bytes)
    layout=f'label: gpt\nlabel-id: {disk_guid}\nunit: sectors\n\nstart={first},size={count},type=U,uuid={part_guid}\n'
    run('sfdisk',disk,input=layout,text=True,stdout=subprocess.DEVNULL)
    run('mkfs.fat','-F','32','--offset',str(first),disk,str(count//2),stdout=subprocess.DEVNULL)
    media=str(disk)+'@@'+str(first*512)
    initrd=out/'INITRD.IMG';shutil.copyfile(desktop/'INITRD.IMG',initrd)
    run('mmd','-i',initrd,'::MEDIA/SYSTEM')
    target=out/'write-target';target.write_text(f'BRKAHCI1\n32902,10530,0,31,2,0,{sectors},512,512\nQEMU HARDDISK\nBROOK-SYSTEM-TEST01\n')
    selector=out/'boot-volume';selector.write_text(f'BRKESP1\n{disk_guid.bytes_le.hex()}\n{part_guid.bytes_le.hex()}\n{first},{count}\n')
    run('mcopy','-i',initrd,target,'::ETC/ahci-write-target')
    run('mcopy','-i',initrd,selector,'::ETC/ahci-boot-volume')
    run('mcopy','-s','-i',media,recovery/'payload/EFI','::/')
    run('mmd','-i',media,'::KERNEL')
    run('mcopy','-i',media,desktop/'kernel.elf','::KERNEL/BROOK.ELF')
    run('mcopy','-i',media,initrd,'::INITRD.IMG')
    cfg=out/'BROOK.CFG';cfg.write_text('TARGET=KERNEL\\BROOK.ELF\nDEBUG_TEXT=1\nDISPLAY_WIDTH=1920\nDISPLAY_HEIGHT=1080\n')
    run('mcopy','-i',media,cfg,'::BROOK.CFG')
    run('mcopy','-i',media,store,'::STORE.IMG')
    run('mcopy','-i',media,desktop/'writable-desktop','::STORE.OK')
    result={'scope':'private SATA-only GPT/FAT/store desktop','phases':[],'desktop_ui_acceptance':False}
    token=uuid.uuid4().hex
    for phase in ('create','verify'):
        base=out/phase;base.mkdir();variables=base/'VARS.fd';shutil.copy2(os.environ['OVMF_VARS'],variables);variables.chmod(0o600)
        with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
        argv=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','8','-m','8192','-nodefaults',
              '-drive','if=pflash,format=raw,readonly=on,file='+os.environ['OVMF_CODE'],
              '-drive','if=pflash,format=raw,file='+str(variables),'-device','VGA,vgamem_mb=64',
              '-drive',f'if=none,id=sata,format=raw,file={disk}',
              '-device','ide-hd,drive=sata,bus=ide.0,bootindex=1,serial=BROOK-SYSTEM-TEST01',
              '-device','qemu-xhci,id=usbc','-device','usb-net,bus=usbc.0,port=2,netdev=network',
              '-netdev',f'user,id=network,restrict=on,hostfwd=tcp:127.0.0.1:{port}-:1234',
              '-display','none','-serial','file:'+str(base/'serial.log'),
              '-qmp','unix:'+str(base/'qmp.sock')+',server=on,wait=off','-no-reboot']
        save(base/'argv.json',argv)
        with (base/'qemu.log').open('x') as log:
            owner=subprocess.Popen(argv,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT);q=None
            try:
                deadline=time.monotonic()+360;serial=''
                while time.monotonic()<deadline:
                    assert owner.poll() is None,'QEMU exited before desktop boot'
                    if (base/'serial.log').exists():serial=(base/'serial.log').read_text(errors='replace')
                    assert 'PANIC_HEX:' not in serial,serial[-4000:]
                    if 'HARDWARE_NETWORK_READY' in serial and 'DEBUG_TCP: listening' in serial:break
                    time.sleep(.2)
                else:raise TimeoutError(serial[-4000:])
                h=Hardware(base/'registry');attached=h.attach('127.0.0.1',port);s=attached['session'];doctor=h.doctor(s)
                save(base/'doctor.json',doctor)
                assert 'ahci system: verified GPT entry' in serial and 'writable desktop: ready' in serial,serial[-5000:]
                assert doctor['guest']['writable_desktop']['ready'] and not doctor['guest']['app_snapshot']['configured'],doctor
                request=uuid.uuid4().hex
                h.guest_run(s,request,['/boot/STOREQA.ELF',phase,token,'system'],cwd='/tmp',uid=1000,wait_s=2)
                deadline=time.monotonic()+120
                while time.monotonic()<deadline:
                    job=h.job(s,request,wait_s=2)
                    if job['state']=='completed':break
                    assert job['state'] not in ('failed','rejected','lost'),job
                else:raise TimeoutError('Inspect existing test job '+request)
                marker='WRITABLE_DESKTOP_CREATE_PASS' if phase=='create' else 'WRITABLE_DESKTOP_REBOOT_PASS'
                save(base/'job.json',job);assert job.get('exit',{}).get('code')==0 and marker in job['stdout']['text'],job
                shutdown=uuid.uuid4().hex;state=h.power(s,shutdown,'shutdown');deadline=time.monotonic()+120
                while state['state']!='ready':
                    assert state['state']!='failed' and time.monotonic()<deadline,state
                    time.sleep(.2);state=h.power(s,shutdown,'status')
                clean=h.command(s,'bridge hardware caps')['writable_desktop'];save(base/'clean.json',clean)
                assert clean['clean_finalized'] and not clean['dirty_session'] and not clean['state_uncertain'] and not clean['final_error'],clean
                q=Qmp(base/'qmp.sock');q.call('quit');assert owner.wait(timeout=15)==0
                result['phases'].append({'operation':phase,'checked_shutdown':True,'clean_finalized':True});save(out/'result.json',result)
            finally:
                if q:q.close()
                if owner.poll() is None:owner.terminate();owner.wait(timeout=15)
    with (out/'gpt-check.log').open('x') as log:run('sfdisk','--verify',disk,stdout=log,stderr=subprocess.STDOUT)
    assert first%2048==0 and count%2048==0
    fat=out/'FAT-check.img';run('dd','if='+str(disk),'of='+str(fat),'bs=1048576',
        'skip='+str(first//2048),'count='+str(count//2048),'conv=sparse','status=none')
    with (out/'fat-check.log').open('x') as log:run('fsck.fat','-n',fat,stdout=log,stderr=subprocess.STDOUT)
    fat.unlink()
    extracted=out/'STORE-recovered.img';run('mcopy','-i',media,'::STORE.IMG',extracted)
    with (out/'e2fsck.log').open('x') as log:run('e2fsck','-fn',extracted,stdout=log,stderr=subprocess.STDOUT)
    with extracted.open('rb') as f:f.seek(1082);assert f.read(2)==b'\1\0'
    extracted.unlink();result.update(passed=True,standard_ext2_check=True,standard_fat_check=True,gpt_check=True,usb_storage_attached=False)
    save(out/'result.json',result);print('SATA_DESKTOP_QEMU_PASS',out,flush=True)


if __name__=='__main__':main()
