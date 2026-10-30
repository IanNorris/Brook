#!/usr/bin/env python3
"""Prepare a RAM-only USB/UEFI bring-up payload and a disposable test disk.

Does not open host devices. Copy payload/ to an existing FAT32 USB partition.
No runtime drivers are included: this first gate tests firmware loading and
kernel entry, not USB input/storage or installed applications.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[1]

def run(*args,**kw):return subprocess.run(list(map(str,args)),check=True,**kw)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build',type=Path,default=ROOT/'build/release')
    ap.add_argument('--output',type=Path,required=True)
    args=ap.parse_args();build=args.build.resolve();out=args.output.resolve()
    for tool in ['mcopy','mmd','mtype','mkfs.fat','sfdisk']:
        if not shutil.which(tool):ap.error('Missing '+tool+'; run inside nix-shell')
    loader=build/'bootloader/BOOTX64.efi';kernel=build/'kernel/BROOK.elf'
    if not loader.is_file() or not kernel.is_file():ap.error('Build bootloader and kernel first')
    out.mkdir(parents=True,exist_ok=False)
    (out/'build/kernel/drivers').mkdir(parents=True) # intentionally empty
    env=dict(os.environ,BROOK_BUILD_DIR=str(out/'build'))
    env.pop('BROOK_PROVIDER_PAYLOAD',None)
    run('bash',ROOT/'scripts/make_initrd.sh','--release',env=env)
    initrd=out/'build/esp/INITRD.IMG'
    run('mmd','-i',initrd,'::BOOT')
    startup=out/'INIT.RC';startup.write_text('set wm\nset vfb none\nkconsole\n')
    run('mcopy','-i',initrd,startup,'::BOOT/INIT.RC')
    payload=out/'payload';(payload/'EFI/BOOT').mkdir(parents=True);(payload/'KERNEL').mkdir()
    shutil.copy2(loader,payload/'EFI/BOOT/BOOTX64.EFI')
    shutil.copy2(kernel,payload/'KERNEL/BROOK.ELF')
    shutil.copy2(initrd,payload/'INITRD.IMG')
    (payload/'BROOK.CFG').write_text('TARGET=KERNEL\\BROOK.ELF\nDEBUG_TEXT=1\nLOG_MEMORY=0\nLOG_INTERRUPTS=0\n')
    (payload/'README.TXT').write_text(
        'Brook XPS 13 9343: first UEFI/kernel bring-up probe\n\n'
        'Use F12 and select the UEFI USB entry; Secure Boot must be disabled\n'
        'for this unsigned development loader. No installation is performed.\n'
        'This probe uses a RAM disk and firmware framebuffer. It includes no\n'
        'runtime USB/input/storage/network/audio/accelerated display drivers.\n'
        'A desktop and kernel log window are the expected success screen.\n'
        'Input may be unavailable; that is outside this first boot gate.\n'
        'If it stops, record the last message/progress and scan panic QR pages.\n'
        'Scanner: https://khione:9001/ or https://192.168.85.8:9001/\n')
    files={str(p.relative_to(payload)):dict(bytes=p.stat().st_size,sha256=sha(p))
           for p in sorted(payload.rglob('*')) if p.is_file()}
    esp=out/'esp.img'
    with esp.open('xb') as f:f.truncate(64*1024*1024)
    run('mkfs.fat','-F','32','-n','BROOK_TEST',esp)
    run('mmd','-i',esp,'::EFI','::EFI/BOOT','::KERNEL')
    for name,row in files.items():
        run('mcopy','-i',esp,payload/name,'::/'+name)
        content=subprocess.check_output(['mtype','-i',str(esp),'::/'+name])
        if hashlib.sha256(content).hexdigest()!=row['sha256']:raise RuntimeError('FAT readback mismatch: '+name)
    disk=out/'probe.raw'
    with disk.open('xb') as f:f.truncate(128*1024*1024)
    run('sfdisk','--no-reread','--no-tell-kernel',disk,
        input='label: gpt\nunit: sectors\n\nstart=2048,size=131072,type=U,name="Brook EFI"\n',text=True)
    with disk.open('r+b') as dst,esp.open('rb') as src:
        dst.seek(1024*1024);shutil.copyfileobj(src,dst)
    (out/'manifest.json').write_text(json.dumps(dict(files=files,kernel_sha256=sha(kernel),
        scope='RAM-only UEFI/kernel handoff; no runtime drivers',test_disk=str(disk)),indent=2)+'\n')
    print('UEFI_PROBE_PREPARED',payload,flush=True)

if __name__=='__main__':main()
