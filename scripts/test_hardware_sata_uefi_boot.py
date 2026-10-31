#!/usr/bin/env python3
"""Boot Brook from a new private QEMU SATA ESP, with no USB storage device.

This is an EFI/kernel/driver baseline, not an installer or desktop-acceptance
test. The disk is a newly created regular file and QEMU uses a snapshot overlay.
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


def run(*args,**kwargs):
    return subprocess.run([str(x) for x in args],cwd=ROOT,check=True,**kwargs)


def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--desktop',required=True,type=Path)
    p.add_argument('--recovery',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path)
    a=p.parse_args();desktop=a.desktop.resolve(strict=True);recovery=a.recovery.resolve(strict=True)
    out=a.output.absolute();out.mkdir(parents=True,exist_ok=False)
    manifest=json.loads((desktop/'manifest.json').read_text())
    for name in ('kernel.elf','INITRD.IMG'):
        assert sha(desktop/name)==manifest['files'][name]['sha256']
    disk=out/'sata.raw';disk_bytes=512*1024**2;partition_sectors=disk_bytes//512-4096
    with disk.open('xb') as f:f.truncate(disk_bytes)
    layout=f'label: dos\nunit: sectors\n\nstart=2048,size={partition_sectors},type=ef,bootable\n'
    run('sfdisk',disk,input=layout,text=True,stdout=subprocess.DEVNULL)
    run('mkfs.fat','-F','32','--offset','2048',disk,str(partition_sectors//2),stdout=subprocess.DEVNULL)
    media=str(disk)+'@@1048576'
    run('mcopy','-s','-i',media,recovery/'payload/EFI','::/')
    run('mmd','-i',media,'::KERNEL')
    run('mcopy','-i',media,desktop/'kernel.elf','::KERNEL/BROOK.ELF')
    run('mcopy','-i',media,desktop/'INITRD.IMG','::INITRD.IMG')
    config=out/'BROOK.CFG';config.write_text('TARGET=KERNEL\\BROOK.ELF\nDEBUG_TEXT=1\nDISPLAY_WIDTH=1024\nDISPLAY_HEIGHT=768\n')
    run('mcopy','-i',media,config,'::BROOK.CFG')
    original=sha(disk);variables=out/'VARS.fd';shutil.copy2(os.environ['OVMF_VARS'],variables);variables.chmod(0o600)
    with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
    argv=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','4','-m','4096','-nodefaults',
          '-drive','if=pflash,format=raw,readonly=on,file='+os.environ['OVMF_CODE'],
          '-drive','if=pflash,format=raw,file='+str(variables),'-device','VGA,vgamem_mb=64',
          '-drive',f'if=none,id=sata,format=raw,snapshot=on,file={disk}',
          '-device','ide-hd,drive=sata,bus=ide.0,bootindex=1,serial=BROOK-BOOT-TEST-01',
          '-device','qemu-xhci,id=usbc','-device','usb-net,bus=usbc.0,port=2,netdev=network',
          '-netdev',f'user,id=network,restrict=on,hostfwd=tcp:127.0.0.1:{port}-:1234',
          '-display','none','-serial','file:'+str(out/'serial.log'),
          '-qmp','unix:'+str(out/'qmp.sock')+',server=on,wait=off','-no-reboot']
    (out/'argv.json').write_text(json.dumps(argv,indent=2)+'\n')
    with (out/'qemu.log').open('x') as log:
        owner=subprocess.Popen(argv,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT);q=None
        try:
            deadline=time.monotonic()+240;serial=''
            while time.monotonic()<deadline:
                assert owner.poll() is None,'QEMU exited before SATA boot'
                if (out/'serial.log').exists():serial=(out/'serial.log').read_text(errors='replace')
                assert 'PANIC_HEX:' not in serial,serial[-4000:]
                if 'HARDWARE_NETWORK_READY' in serial and 'DEBUG_TCP: listening' in serial:break
                time.sleep(.2)
            else:raise TimeoutError(serial[-4000:])
            h=Hardware(out/'registry');attached=h.attach('127.0.0.1',port);s=attached['session']
            status=h.command(s,'bridge hardware ahci-ro')
            observer=h.command(s,'bridge hardware ahci')
            (out/'ahci.json').write_text(json.dumps({'status':status,'observer':observer},indent=2)+'\n')
            assert status['ready'] and status['sectors']==disk_bytes//512,status
            assert status['reason']=='ATA write target rejected; device read-only',status
            verified=[]
            with disk.open('rb') as source:
                for lba in (0,1,2048,4096,disk_bytes//512-1):
                    source.seek(lba*512);expected=source.read(512)
                    actual=h.command(s,f'bridge hardware ahci-sector {lba}')
                    assert bytes.fromhex(actual.get('hex',''))==expected,(lba,actual)
                    verified.append(lba)
            request=uuid.uuid4().hex;state=h.power(s,request,'shutdown');deadline=time.monotonic()+120
            while state['state']!='ready':
                assert state['state']!='failed' and time.monotonic()<deadline,state
                time.sleep(.2);state=h.power(s,request,'status')
            q=Qmp(out/'qmp.sock');q.call('quit');assert owner.wait(timeout=15)==0
        finally:
            if q:q.close()
            if owner.poll() is None:owner.terminate();owner.wait(timeout=15)
    assert sha(disk)==original,'SATA source image changed'
    result={'passed':True,'scope':'UEFI-to-kernel boot from SATA, no USB storage; RAM desktop only',
            'ahci':status,'disk_sha256':original,'verified_sectors':verified,'checked_shutdown':True,
            'desktop_and_apps_verified':False,'installer_verified':False}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print('SATA_UEFI_BOOT_PASS',out,flush=True)


if __name__=='__main__':main()
