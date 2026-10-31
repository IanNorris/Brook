#!/usr/bin/env python3
"""Boot a completed private installer target without USB storage in QEMU."""
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
def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def dell_fixture_sha(path,desktop):
    # Bounded evidence only: GPT metadata, installed payload and one gap.
    # QEMU uses snapshot=on, so the base is not its writable overlay.
    digest=hashlib.sha256()
    payload_sectors=12582912 if desktop else 126976
    ranges=((0,34*512),(100*512,512),(2048*512,payload_sectors*512),((500118192-33)*512,33*512))
    with path.open('rb') as f:
        for offset,length in ranges:
            f.seek(offset);remaining=length
            while remaining:
                data=f.read(min(1024*1024,remaining))
                if not data:raise ValueError('short sparse fixture')
                digest.update(data);remaining-=len(data)
    return digest.hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
    p.add_argument('--desktop-store',action='store_true',help='Verify the full desktop system store is admitted')
    p.add_argument('--dell-capacity-fixture',action='store_true',help='Use the sparse Dell-capacity test fixture and bounded base-disk checks')
    a=p.parse_args();image=a.image.resolve(strict=True)
    expected=500118192*512 if a.dell_capacity_fixture else 6*1024**3+2*1024**2 if a.desktop_store else 64*1024**2
    if a.image.is_symlink() or not image.is_file() or image.stat().st_size!=expected:
        p.error('Expected the specified private installer target size')
    checksum=(lambda path: dell_fixture_sha(path,a.desktop_store)) if a.dell_capacity_fixture else sha
    out=a.output.absolute();out.mkdir(parents=True,exist_ok=False);original=checksum(image)
    variables=out/'VARS.fd';shutil.copy2(os.environ['OVMF_VARS'],variables);variables.chmod(0o600)
    with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
    argv=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','4','-m','4096','-nodefaults',
          '-drive','if=pflash,format=raw,readonly=on,file='+os.environ['OVMF_CODE'],
          '-drive','if=pflash,format=raw,file='+str(variables),'-device','VGA,vgamem_mb=64',
          '-drive',f'if=none,id=sata,format=raw,snapshot=on,file={image}',
          '-device','ide-hd,drive=sata,bus=ide.0,bootindex=1,serial='+('BROOK-SYSTEM-TEST01' if a.desktop_store else 'BROOK-AHCI-TEST-01'),
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
                assert owner.poll() is None,'QEMU exited before boot'
                if (out/'serial.log').exists():serial=(out/'serial.log').read_text(errors='replace')
                assert 'PANIC_HEX:' not in serial,serial[-4000:]
                if 'HARDWARE_NETWORK_READY' in serial and 'DEBUG_TCP: listening' in serial and 'BOOT: desktop presented' in serial:break
                time.sleep(.2)
            else:raise TimeoutError(serial[-4000:])
            h=Hardware(out/'registry');attached=h.attach('127.0.0.1',port);s=attached['session'];status=h.command(s,'bridge hardware ahci-ro')
            assert status['ready'] and status['sectors']==expected//512,status
            if a.desktop_store:
                doctor=h.doctor(s);(out/'doctor.json').write_text(json.dumps(doctor,indent=2)+'\n')
                assert doctor['guest']['writable_desktop']['ready'] and not doctor['guest']['app_snapshot']['configured'],doctor
            else:assert status['reason']=='read-only ATA device ready',status
            request=uuid.uuid4().hex;power=h.power(s,request,'shutdown');deadline=time.monotonic()+120
            while power['state']!='ready':
                assert power['state']!='failed' and time.monotonic()<deadline,power
                time.sleep(.2);power=h.power(s,request,'status')
            q=Qmp(out/'qmp.sock');q.call('quit');assert owner.wait(timeout=15)==0
        finally:
            if q:q.close()
            if owner.poll() is None:owner.terminate();owner.wait(timeout=15)
    assert checksum(image)==original
    (out/'result.json').write_text(json.dumps({'passed':True,'usb_storage_attached':False,('disk_sample_sha256' if a.dell_capacity_fixture else 'disk_sha256'):original,
        'scope':('installed desktop system store admission; no UI acceptance' if a.desktop_store else
                 'installed kernel boot only; no application store or full desktop acceptance')},indent=2)+'\n')
    print('INSTALLED_SATA_QEMU_BOOT_PASS',flush=True)
if __name__=='__main__':main()
