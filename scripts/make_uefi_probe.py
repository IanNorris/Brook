#!/usr/bin/env python3
"""Prepare a RAM-only USB/UEFI bring-up payload and a disposable test disk.

Does not open host devices. Copy payload/ to an existing FAT32 USB partition.
The default payload has no runtime modules; --driver opts into individual
bring-up drivers. Built-in PS/2 fallbacks may still provide input.
"""
import argparse
import hashlib
import json
import os
import re
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
    ap.add_argument('--driver',action='append',default=[],help='Include a named built module; default none')
    ap.add_argument('--usb-read-only',action='store_true')
    ap.add_argument('--media-mount',help='Mount marked USB filesystem at this path')
    ap.add_argument('--partition-table',choices=['gpt','mbr'],default='gpt')
    ap.add_argument('--input-probe',action='store_true',help='Request <=1280x720 and display keyboard/frame diagnostics')
    ap.add_argument('--display-width',type=int,default=0)
    ap.add_argument('--display-height',type=int,default=0)
    args=ap.parse_args()
    if bool(args.display_width)!=bool(args.display_height) or not (0<=args.display_width<=16384 and 0<=args.display_height<=16384):
        ap.error('Set both display bounds between 1 and 16384')
    display_width=args.display_width or (1280 if args.input_probe else 0)
    display_height=args.display_height or (720 if args.input_probe else 0)
    build=args.build.resolve();out=args.output.resolve()
    for tool in ['mcopy','mmd','mtype','mkfs.fat','sfdisk']:
        if not shutil.which(tool):ap.error('Missing '+tool+'; run inside nix-shell')
    loader=build/'bootloader/BOOTX64.efi';kernel=build/'kernel/BROOK.elf'
    if not loader.is_file() or not kernel.is_file():ap.error('Build bootloader and kernel first')
    for name in args.driver:
        if not re.fullmatch('[a-z0-9_]+',name) or not (build/'kernel/drivers'/f'{name}.mod').is_file():ap.error('Invalid/missing driver: '+name)
    if args.media_mount and (not args.media_mount.startswith('/') or '..' in args.media_mount.split('/') or len(args.media_mount)>63):ap.error('Invalid media mount path')
    out.mkdir(parents=True,exist_ok=False)
    drivers=out/'build/kernel/drivers';drivers.mkdir(parents=True)
    for name in args.driver:shutil.copy2(build/'kernel/drivers'/f'{name}.mod',drivers/f'{name}.mod')
    env=dict(os.environ,BROOK_BUILD_DIR=str(out/'build'))
    env.pop('BROOK_PROVIDER_PAYLOAD',None)
    run('bash',ROOT/'scripts/make_initrd.sh','--release',env=env)
    initrd=out/'build/esp/INITRD.IMG'
    marker=out/'audio-muted';marker.write_text('1\n');run('mcopy','-i',initrd,marker,'::ETC/audio-muted')
    if args.usb_read_only:
        marker=out/'usb-read-only';marker.write_text('1\n');run('mcopy','-i',initrd,marker,'::ETC/usb-read-only')
    if args.media_mount:
        current=''
        for part in args.media_mount.strip('/').split('/'):
            current+='/'+part
            probe=subprocess.run(['mdir','-i',str(initrd),'::'+current],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            if probe.returncode:run('mmd','-i',initrd,'::'+current)
    run('mmd','-i',initrd,'::BOOT')
    startup=out/'INIT.RC';startup.write_text('set wm\nset vfb none\nkconsole\n'+('hwprobe on\n' if args.input_probe else ''))
    run('mcopy','-i',initrd,startup,'::BOOT/INIT.RC')
    payload=out/'payload';(payload/'EFI/BOOT').mkdir(parents=True);(payload/'KERNEL').mkdir()
    shutil.copy2(loader,payload/'EFI/BOOT/BOOTX64.EFI')
    shutil.copy2(kernel,payload/'KERNEL/BROOK.ELF')
    shutil.copy2(initrd,payload/'INITRD.IMG')
    (payload/'BROOK.CFG').write_text('TARGET=KERNEL\\BROOK.ELF\nDEBUG_TEXT=1\nLOG_MEMORY=0\nLOG_INTERRUPTS=0\n'+(f'DISPLAY_WIDTH={display_width}\nDISPLAY_HEIGHT={display_height}\n' if display_width else ''))
    (payload/'README.TXT').write_text(
        'Brook XPS 13 9343: first UEFI/kernel bring-up probe\n\n'
        'Use F12 and select the UEFI USB entry; Secure Boot must be disabled\n'
        'for this unsigned development loader. No installation is performed.\n'
        'This probe uses a RAM disk and firmware framebuffer.\n'
        f'Runtime modules: {", ".join(args.driver) or "none"}. Built-in PS/2 input is retained.\n'
        'A desktop and kernel log window are the expected success screen.\n'
        'Input may be unavailable; that is outside this first boot gate.\n'
        'If it stops, record the last message/progress and scan panic QR pages.\n'
        'Scanner: https://khione:9001/ or https://192.168.85.8:9001/\n')
    if args.input_probe:
        with (payload/'README.TXT').open('a') as f:
            f.write(f'\nInput probe: requests <={display_width}x{display_height} GOP; fallback is smallest mode.\n'
                    'Press plain A several times; HWKEY/HWKBD counters should change.\n' +
                    ('USB input requires the included xHCI driver.\n' if 'xhci' in args.driver else
                     'External USB keyboards require an xHCI driver payload.\n'))
    if args.media_mount:(payload/'BROOK.MNT').write_text(args.media_mount+'\n')
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
    partition=('label: gpt\nunit: sectors\n\nstart=2048,size=131072,type=U,name="Brook EFI"\n' if args.partition_table=='gpt' else
               'label: dos\nunit: sectors\n\nstart=2048,size=131072,type=c,bootable\n')
    run('sfdisk','--no-reread','--no-tell-kernel',disk,input=partition,text=True)
    with disk.open('r+b') as dst,esp.open('rb') as src:
        dst.seek(1024*1024);shutil.copyfileobj(src,dst)
    (out/'manifest.json').write_text(json.dumps(dict(files=files,kernel_sha256=sha(kernel),
        scope='UEFI probe; drivers='+','.join(args.driver),usb_read_only=args.usb_read_only,partition_table=args.partition_table,test_disk=str(disk)),indent=2)+'\n')
    print('UEFI_PROBE_PREPARED',payload,flush=True)

if __name__=='__main__':main()
