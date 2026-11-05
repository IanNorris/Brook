#!/usr/bin/env python3
"""End-to-end panic journal retrieval via the physical bridge against private QEMU."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'tools/vm_bridge')]
from hardware import Hardware


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1',0));return s.getsockname()[1]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',type=Path,required=True);p.add_argument('--kernel',type=Path,required=True)
    p.add_argument('--xhci',type=Path,required=True);p.add_argument('--firmware',type=Path,required=True)
    p.add_argument('--cc',required=True);p.add_argument('--cxx',required=True)
    a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='brook-panic-bridge-') as tmp:
        d=Path(tmp);disk=d/'disk.raw';shutil.copyfile(a.image.resolve(strict=True),disk)
        media=str(disk)+'@@1048576';initrd=d/'initrd.img'
        def run(*args):subprocess.run(list(map(str,args)),check=True,stdout=subprocess.DEVNULL)
        run('mcopy','-i',media,'::/INITRD.IMG',initrd)
        for name in ('DATA','MEDIA','MEDIA/SYSTEM'):
            exists=subprocess.run(['mdir','-i',str(initrd),'::/'+name],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            if exists.returncode:run('mmd','-i',initrd,'::/'+name)
        bulk=d/'HWBULK.ELF';exporter=d/'PANICLOG.ELF'
        run(a.cc,'-static','-O2','-Wall','-Wextra','-Werror',ROOT/'tools/hardware_bulk.c','-o',bulk)
        run(a.cxx,'-static','-std=c++17','-O2','-Wall','-Wextra','-Werror',ROOT/'tools/panic_journal_export.cpp','-o',exporter)
        for src,target in ((bulk,'::/BOOT/HWBULK.ELF'),(exporter,'::/BOOT/PANICLOG.ELF'),(a.xhci.resolve(strict=True),'::/DRIVERS/XHCI.MOD')):
            run('mcopy','-o','-i',initrd,src,target)
        key=d/'key';key.write_text('b'*32);key.chmod(0o600)
        for name,value in (('hardware-update-key','b'*32),('hardware-maintenance','1\n'),('hardware-bulk','1\n')):
            f=d/name;f.write_text(value);run('mcopy','-o','-i',initrd,f,'::/ETC/'+name)
        rc=d/'INIT.RC';rc.write_text('set wm\nset vfb none\n')
        run('mcopy','-o','-i',initrd,rc,'::/BOOT/INIT.RC')
        message=b'integrated bridge panic'
        packet=struct.pack('<BBBBI',0x2d,1,0,1,0xcafef00d)+struct.pack('<II',0xa3000009,len(message))+message
        journal=bytearray(258*512);header=bytearray(512);header[:8]=b'BRKPAN01'
        struct.pack_into('<IIQI',header,8,1,len(packet),7,zlib.crc32(packet));struct.pack_into('<I',header,28,zlib.crc32(header))
        journal[:512]=header;journal[512:512+len(packet)]=packet
        fixture=d/'PANIC.BIN';fixture.write_bytes(journal);run('mcopy','-i',initrd,fixture,'::/MEDIA/SYSTEM/PANIC.BIN')
        run('mcopy','-o','-i',media,initrd,'::/INITRD.IMG')
        run('mcopy','-o','-i',media,a.kernel.resolve(strict=True),'::/KERNEL/BROOK.ELF')
        firmware=a.firmware.resolve(strict=True);variables=d/'vars.fd';shutil.copyfile(firmware/'OVMF_VARS.fd',variables)
        debug_port,bulk_port=free_port(),free_port()
        if debug_port==bulk_port:raise RuntimeError('duplicate host ports')
        serial=d/'serial.log'
        args=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','2','-m','1024','-nodefaults',
              '-drive',f'if=pflash,format=raw,readonly=on,file={firmware / "OVMF_CODE.fd"}',
              '-drive',f'if=pflash,format=raw,file={variables}','-device','VGA','-device','qemu-xhci,id=usb',
              '-drive',f'if=none,id=boot,format=raw,file={disk}',
              '-device','usb-storage,bus=usb.0,port=1,drive=boot,bootindex=1',
              '-device','usb-net,bus=usb.0,port=2,netdev=net',
              '-netdev',f'user,id=net,restrict=on,hostfwd=tcp:127.0.0.1:{debug_port}-:1234,hostfwd=tcp:127.0.0.1:{bulk_port}-:1236',
              '-display','none','-serial',f'file:{serial}','-no-reboot']
        with (d/'qemu.log').open('wb') as log:
            proc=subprocess.Popen(args,stdout=subprocess.DEVNULL,stderr=log)
            try:
                deadline=time.monotonic()+90
                while time.monotonic()<deadline:
                    text=serial.read_text(errors='replace') if serial.exists() else ''
                    if 'DEBUG_TCP: listening on port 1234' in text:break
                    if proc.poll() is not None:raise RuntimeError('QEMU exited: '+text[-3000:])
                    time.sleep(.1)
                else:raise RuntimeError('debug listener timed out: '+text[-3000:])
                bridge=Hardware(registry=d/'registry');session=bridge.attach('127.0.0.1',debug_port)['session']
                result=bridge.panic_report(session,'a'*32,key,d/'result',bulk_port)
                assert result['sequence']==7 and Path(result['artifact_path']).read_bytes()==packet
                assert result['sha256']==hashlib.sha256(packet).hexdigest() and result['bytes']==len(packet)
                receipt=json.loads((d/'result'/'receipt.json').read_text())
                assert receipt['state']=='completed' and receipt['guest_boot_id']==bridge.load_session(session)['guest_boot_id']
                print('hardware panic bridge end-to-end: PASS')
            finally:
                if proc.poll() is None:proc.terminate();proc.wait(timeout=10)

if __name__=='__main__':main()
