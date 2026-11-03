#!/usr/bin/env python3
"""Exercise the read-only EFI inspector on disposable, private SATA copies."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import uuid


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','probe','ovmf-code','ovmf-vars','output'):
        p.add_argument('--'+name,required=True,type=Path)
    a=p.parse_args()
    source=a.image.resolve(strict=True)
    if a.image.is_symlink() or not source.is_file() or source.stat().st_size!=128*1024*1024:
        p.error('expected the disposable 128 MiB GPT fixture, not a device')
    a.output.mkdir(parents=True,exist_ok=False)
    result={}
    for scenario in ('clean','dirty','wrong-uuid','bad-gpt','duplicate-disk'):
        with tempfile.TemporaryDirectory(prefix='probe-',dir=a.output) as tmp:
            root=Path(tmp);disk=root/'disk.raw';varsfile=root/'vars.fd';log=root/'debug.log'
            subprocess.run(['cp','--reflink=auto','--sparse=always',source,disk],check=True)
            subprocess.run(['mcopy','-o','-i',str(disk)+'@@1048576',str(a.probe),'::/EFI/BOOT/BOOTX64.EFI'],check=True)
            # Build the same strict 4 KiB ext2 profile as the direct SSD path,
            # preserving the fixture's filesystem UUID in this private copy.
            partition=root/'data.img'
            with disk.open('rb') as f:
                f.seek(133120*512+1024+104);fs_uuid=uuid.UUID(bytes=f.read(16))
            with partition.open('wb') as f:f.truncate(126976*512)
            subprocess.run(['mke2fs','-q','-F','-t','ext2','-b','4096','-I','128','-N','1024',
                            '-m','0','-O','none,filetype,sparse_super,large_file','-U',str(fs_uuid),str(partition)],
                           check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            with disk.open('r+b') as target,partition.open('rb') as data:
                target.seek(133120*512);shutil.copyfileobj(data,target,1024*1024)
            partition.unlink()
            if scenario=='duplicate-disk':
                secondBase=root/'second-base.raw'
                subprocess.run(['cp','--reflink=auto','--sparse=always',disk,secondBase],check=True)
            pristine=digest(disk)
            with disk.open('r+b') as f:
                if scenario in ('dirty','duplicate-disk'):f.seek(133120*512+1024+58);f.write(b'\0\0')
                if scenario=='wrong-uuid':f.seek(133120*512+1024+104);f.write(b'\xff')
                if scenario=='bad-gpt':
                    # Corrupt only the backup entry array, leaving firmware's
                    # primary GPT path intact so the probe can actually boot.
                    f.seek(262111*512+4);old=f.read(1);f.seek(262111*512+4);f.write(bytes([old[0]^1]))
            before=digest(disk);shutil.copyfile(a.ovmf_vars,varsfile)
            overlay=root/'overlay.qcow2'
            subprocess.run(['qemu-img','create','-q','-f','qcow2','-F','raw','-b',str(disk),str(overlay)],check=True)
            argv=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','2','-m','1024','-nodefaults',
                  '-drive',f'if=pflash,format=raw,readonly=on,file={a.ovmf_code}',
                  '-drive',f'if=pflash,format=raw,file={varsfile}', '-device','VGA',
                  '-drive',f'if=none,id=main,format=qcow2,file={overlay}',
                  '-device','ide-hd,drive=main,bus=ide.0,bootindex=1',
                  '-display','none','-debugcon',f'file:{log}','-global','isa-debugcon.iobase=0xe9','-no-reboot']
            if scenario=='duplicate-disk':
                second=root/'second.qcow2'
                subprocess.run(['qemu-img','create','-q','-f','qcow2','-F','raw','-b',str(secondBase),str(second)],check=True)
                argv += ['-drive',f'if=none,id=second,format=qcow2,file={second}',
                         '-device','ide-hd,drive=second,bus=ide.1']
            expected={'clean':'CLEAN','dirty':'DIRTY','wrong-uuid':'INVALID','bad-gpt':'INVALID','duplicate-disk':'DIRTY'}[scenario]
            with (a.output/(scenario+'-qemu.log')).open('wb') as qlog:
                proc=subprocess.Popen(argv,stdin=subprocess.DEVNULL,stdout=qlog,stderr=subprocess.STDOUT)
                try:
                    deadline=time.monotonic()+60;observed=''
                    while time.monotonic()<deadline:
                        if log.exists():observed=log.read_text(errors='replace')
                        if any('BROOK_PROBE_'+state+'\n' in observed for state in ('CLEAN','DIRTY','INVALID')):break
                        if proc.poll() is not None:raise RuntimeError(f'{scenario}: QEMU exited {proc.returncode}')
                        time.sleep(.1)
                    else:raise TimeoutError(f'{scenario}: no probe result')
                finally:
                    if proc.poll() is None:proc.terminate()
                    try:proc.wait(timeout=10)
                    except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=10)
            if digest(disk)!=before:raise RuntimeError(f'{scenario}: source fixture changed')
            after=root/'after.raw';subprocess.run(['qemu-img','convert','-f','qcow2','-O','raw',str(overlay),str(after)],check=True)
            lines=observed.strip().splitlines()
            if len(lines)!=2 or lines[0]!='BROOK_PROBE_PARENT_FOUND' or lines[1] not in ('BROOK_PROBE_CLEAN','BROOK_PROBE_DIRTY','BROOK_PROBE_INVALID'):
                raise RuntimeError(f'{scenario}: incomplete or ambiguous probe markers {observed!r}')
            repaired=False
            if scenario=='bad-gpt' and 'BROOK_PROBE_CLEAN\n' in observed:
                offset=262111*512+4
                with source.open('rb') as original,disk.open('rb') as corrupt,after.open('rb') as seen:
                    for stream in (original,corrupt,seen):stream.seek(offset)
                    originalByte,corruptByte,seenByte=(stream.read(1) for stream in (original,corrupt,seen))
                repaired=originalByte!=corruptByte and seenByte==originalByte and digest(after)==pristine
            if 'BROOK_PROBE_'+expected+'\n' not in observed and not repaired:
                raise RuntimeError(f'{scenario}: {observed!r}; backup repair observed={repaired}')
            overlayUnchanged=digest(after)==before
            if not overlayUnchanged and not repaired:raise RuntimeError(f'{scenario}: guest modified its disk overlay')
            if scenario=='duplicate-disk':
                secondAfter=root/'second-after.raw'
                subprocess.run(['qemu-img','convert','-f','qcow2','-O','raw',str(second),str(secondAfter)],check=True)
                if digest(secondAfter)!=digest(secondBase):raise RuntimeError('second guest disk overlay changed')
            result[scenario]={'expected':expected,'debug':observed.strip(),'fixture_unchanged':True,
                              'overlay_unchanged':overlayUnchanged,'ovmf_repaired_backup_before_probe':repaired}
            (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
            print('RECOVERY_FIRMWARE_'+scenario.upper().replace('-','_')+
                  ('_OVMF_REPAIRED_NOT_TESTED' if repaired else '_PASS'),flush=True)


if __name__=='__main__':main()
