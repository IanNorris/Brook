#!/usr/bin/env python3
"""Check SATA boot rejects a bad selector or USB mount collision in QEMU."""
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

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',required=True,type=Path,help='Completed private SATA desktop test directory')
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('--case',required=True,choices=['wrong-guid','usb-collision'])
    p.add_argument('--usb-template',type=Path)
    a=p.parse_args();source=a.source.resolve(strict=True);out=a.output.absolute()
    evidence=json.loads((source/'result.json').read_text())
    assert evidence.get('passed') and evidence.get('usb_storage_attached') is False
    source_disk=source/'sata.raw';assert source_disk.is_file() and not source_disk.is_symlink()
    assert source_disk.stat().st_size==6*1024**3+2*1024**2
    out.mkdir(parents=True,exist_ok=False);disk=out/'sata.raw';run('cp','--reflink=auto','--',source_disk,disk)
    media=str(disk)+'@@1048576'
    if a.case=='wrong-guid':
        initrd=out/'INITRD.IMG';run('mcopy','-i',media,'::INITRD.IMG',initrd)
        volume=out/'selector';run('mcopy','-i',initrd,'::ETC/ahci-boot-volume',volume)
        record=bytearray(volume.read_bytes());assert record.startswith(b'BRKESP1\n')
        record[8]=ord('1') if record[8]!=ord('1') else ord('2');volume.write_bytes(record)
        run('mcopy','-o','-i',initrd,volume,'::ETC/ahci-boot-volume')
        run('mcopy','-o','-i',media,initrd,'::INITRD.IMG')
    usb=None
    if a.case=='usb-collision':
        if not a.usb_template:raise ValueError('usb-collision needs a stopped private USB test disk')
        original=a.usb_template.resolve(strict=True)
        if a.usb_template.is_symlink() or not original.is_file():raise ValueError('USB fixture must be a regular file')
        usb=out/'decoy.raw';run('cp','--reflink=auto','--',original,usb)
        marker=out/'BROOK.MNT';marker.write_text('/media/system\n')
        run('mcopy','-o','-i',str(usb)+'@@1048576',marker,'::BROOK.MNT')
    before=sha(disk);variables=out/'VARS.fd';shutil.copy2(os.environ['OVMF_VARS'],variables);variables.chmod(0o600)
    with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
    argv=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','8','-m','8192','-nodefaults',
          '-drive','if=pflash,format=raw,readonly=on,file='+os.environ['OVMF_CODE'],
          '-drive','if=pflash,format=raw,file='+str(variables),'-device','VGA,vgamem_mb=64',
          '-drive',f'if=none,id=sata,format=raw,file={disk}',
          '-device','ide-hd,drive=sata,bus=ide.0,bootindex=1,serial=BROOK-SYSTEM-TEST01',
          '-device','qemu-xhci,id=usbc','-device','usb-net,bus=usbc.0,port=2,netdev=network',
          '-netdev',f'user,id=network,restrict=on,hostfwd=tcp:127.0.0.1:{port}-:1234',
          '-display','none','-serial','file:'+str(out/'serial.log'),
          '-qmp','unix:'+str(out/'qmp.sock')+',server=on,wait=off','-no-reboot']
    if usb:argv+=['-drive',f'if=none,id=decoy,format=raw,snapshot=on,file={usb}',
                  '-device','usb-storage,bus=usbc.0,port=1,drive=decoy']
    (out/'argv.json').write_text(json.dumps(argv,indent=2)+'\n')
    with (out/'qemu.log').open('x') as log:
        owner=subprocess.Popen(argv,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT);q=None
        try:
            end=time.monotonic()+360;serial=''
            while time.monotonic()<end:
                assert owner.poll() is None,'QEMU exited before maintenance boot'
                if (out/'serial.log').exists():serial=(out/'serial.log').read_text(errors='replace')
                assert 'PANIC_HEX:' not in serial,serial[-4000:]
                if 'HARDWARE_NETWORK_READY' in serial and 'DEBUG_TCP: listening' in serial:break
                time.sleep(.2)
            else:raise TimeoutError(serial[-4000:])
            expected='GPT validation or selection failed' if a.case=='wrong-guid' else 'mount namespace failed'
            assert expected in serial and 'writable desktop: ready' not in serial,serial[-5000:]
            h=Hardware(out/'registry');attached=h.attach('127.0.0.1',port);s=attached['session'];caps=h.command(s,'bridge hardware caps')
            assert not caps['writable_desktop']['ready'] and not caps['app_snapshot']['configured'],caps
            request=uuid.uuid4().hex;state=h.power(s,request,'shutdown');end=time.monotonic()+120
            while state['state']!='ready':
                assert state['state']!='failed' and time.monotonic()<end,state
                time.sleep(.2);state=h.power(s,request,'status')
            q=Qmp(out/'qmp.sock');q.call('quit');assert owner.wait(timeout=15)==0
        finally:
            if q:q.close()
            if owner.poll() is None:owner.terminate();owner.wait(timeout=15)
    after=sha(disk);assert before==after,'rejected boot modified SATA disk'
    (out/'result.json').write_text(json.dumps({'passed':True,'case':a.case,'sata_sha256':before,'no_sata_writes':True},indent=2)+'\n')
    print('SATA_REJECTION_QEMU_PASS',a.case,flush=True)
if __name__=='__main__':main()
