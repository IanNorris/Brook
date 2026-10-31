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
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
    a=p.parse_args();image=a.image.resolve(strict=True)
    if a.image.is_symlink() or not image.is_file() or image.stat().st_size!=64*1024**2:
        p.error('Expected the regular 64 MiB private installer target')
    out=a.output.absolute();out.mkdir(parents=True,exist_ok=False);original=sha(image)
    variables=out/'VARS.fd';shutil.copy2(os.environ['OVMF_VARS'],variables);variables.chmod(0o600)
    with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
    argv=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','4','-m','4096','-nodefaults',
          '-drive','if=pflash,format=raw,readonly=on,file='+os.environ['OVMF_CODE'],
          '-drive','if=pflash,format=raw,file='+str(variables),'-device','VGA,vgamem_mb=64',
          '-drive',f'if=none,id=sata,format=raw,snapshot=on,file={image}',
          '-device','ide-hd,drive=sata,bus=ide.0,bootindex=1,serial=BROOK-AHCI-TEST-01',
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
            assert status['ready'] and status['sectors']==131072 and status['reason']=='read-only ATA device ready',status
            request=uuid.uuid4().hex;power=h.power(s,request,'shutdown');deadline=time.monotonic()+120
            while power['state']!='ready':
                assert power['state']!='failed' and time.monotonic()<deadline,power
                time.sleep(.2);power=h.power(s,request,'status')
            q=Qmp(out/'qmp.sock');q.call('quit');assert owner.wait(timeout=15)==0
        finally:
            if q:q.close()
            if owner.poll() is None:owner.terminate();owner.wait(timeout=15)
    assert sha(image)==original
    (out/'result.json').write_text(json.dumps({'passed':True,'usb_storage_attached':False,'disk_sha256':original,
        'scope':'installed kernel boot only; no application store or full desktop acceptance'},indent=2)+'\n')
    print('INSTALLED_SATA_QEMU_BOOT_PASS',flush=True)
if __name__=='__main__':main()
