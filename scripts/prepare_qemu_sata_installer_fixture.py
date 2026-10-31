#!/usr/bin/env python3
"""Build a small disposable GPT/FAT UEFI source for QEMU installer tests.

This is a kernel boot fixture, not a complete desktop installation image.
Only a new regular file under a new output directory is created.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import uuid
ROOT=Path(__file__).resolve().parents[1]
def run(*args,**kwargs):return subprocess.run([str(x) for x in args],cwd=ROOT,check=True,**kwargs)
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel',required=True,type=Path);p.add_argument('--initrd',required=True,type=Path)
    p.add_argument('--recovery',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
    a=p.parse_args();kernel=a.kernel.resolve(strict=True);initrd=a.initrd.resolve(strict=True);recovery=a.recovery.resolve(strict=True)
    out=a.output.absolute();out.mkdir(parents=True,exist_ok=False);disk=out/'source.raw'
    sectors=131072;first=2048;count=sectors-4096
    with disk.open('xb') as f:f.truncate(sectors*512)
    disk_guid=uuid.uuid4();part_guid=uuid.uuid4()
    layout=f'label: gpt\nlabel-id: {disk_guid}\nunit: sectors\n\nstart={first},size={count},type=U,uuid={part_guid}\n'
    run('sfdisk',disk,input=layout,text=True,stdout=subprocess.DEVNULL)
    run('mkfs.fat','-F','32','-s','1','--offset',str(first),disk,str(count//2),stdout=subprocess.DEVNULL)
    media=str(disk)+'@@'+str(first*512)
    run('mcopy','-s','-i',media,recovery/'payload/EFI','::/')
    run('mmd','-i',media,'::KERNEL')
    run('mcopy','-i',media,kernel,'::KERNEL/BROOK.ELF')
    run('mcopy','-i',media,initrd,'::INITRD.IMG')
    config=out/'BROOK.CFG';config.write_text('TARGET=KERNEL\\BROOK.ELF\nDEBUG_TEXT=1\nDISPLAY_WIDTH=1024\nDISPLAY_HEIGHT=768\n')
    run('mcopy','-i',media,config,'::BROOK.CFG')
    with disk.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
    (out/'manifest.json').write_text(json.dumps({'bytes':disk.stat().st_size,'sha256':digest,'scope':'private QEMU kernel boot fixture'},indent=2)+'\n')
    print('QEMU_INSTALL_SOURCE_READY',out,flush=True)
if __name__=='__main__':main()
