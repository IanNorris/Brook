#!/usr/bin/env python3
"""Test experimental guarded EFI data dispatch on disposable SATA images."""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path
import zlib
from test_recovery_menu_boot import boot
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from hardware_update_protocol import record as slot_record


def sha(path):
    with path.open('rb') as source:return hashlib.file_digest(source,'sha256').hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','efi','firmware','output'):p.add_argument('--'+name,required=True,type=Path)
    p.add_argument('--identity',required=True)
    p.add_argument('--recovery-pair',type=Path,help='Exact prepared read-only guard pair for end-to-end testing')
    a=p.parse_args();source=a.image.resolve(strict=True)
    if a.image.is_symlink() or not source.is_file() or source.stat().st_size!=128*1024*1024:
        p.error('expected the disposable 128 MiB fixture')
    if len(a.identity)!=96 or any(c not in '0123456789abcdef' for c in a.identity):p.error('invalid identity')
    a.output.mkdir(parents=True,exist_ok=False)
    scenarios=('dirty','pending','wrong-uuid','wrong-ata','checker-corrupt') if a.recovery_pair else ('clean','dirty','wrong-uuid','pending','slot-clean','slot-dirty','missing-recovery','corrupt-recovery')
    pair=None
    if a.recovery_pair:
        pair=a.recovery_pair.resolve(strict=True);manifest=json.loads((pair/'manifest.json').read_text())
        if manifest.get('profile')!='qemu-fixture' or manifest.get('guard_identity')!=a.identity:
            p.error('expected an identity-matched QEMU-only guard pair')
        for name in ('KERNEL.ELF','INITRD.IMG','SELECT.BIN'):
            item=pair/name;meta=manifest['files'][name]
            if item.is_symlink() or not item.is_file() or item.stat().st_size!=meta['bytes'] or sha(item)!=meta['sha256']:
                p.error('guard pair manifest mismatch: '+name)
    for scenario in scenarios:
        # QEMU's Unix monitor socket has a short path-length limit.
        with tempfile.TemporaryDirectory(prefix='brk-dispatch-') as tmp:
            work=Path(tmp);disk=work/'disk.raw';partition=work/'data.img'
            subprocess.run(['cp','--reflink=auto','--sparse=always',source,disk],check=True)
            media=str(disk)+'@@1048576'
            subprocess.run(['mcopy','-o','-i',media,str(a.efi),'::/EFI/BOOT/BOOTX64.EFI'],check=True)
            if pair:
                replacements={name:pair/name for name in ('KERNEL.ELF','INITRD.IMG','SELECT.BIN')}
                expectedHashes={name:manifest['files'][name]['sha256'] for name in replacements}
                if scenario=='checker-corrupt':
                    initrd=work/'tampered-initrd.img';shutil.copyfile(pair/'INITRD.IMG',initrd)
                    checker=bytearray(subprocess.check_output(['mtype','-i',str(initrd),'::/BOOT/FATFSCK.ELF']))
                    checker[-1]^=1;bad=work/'tampered-checker';bad.write_bytes(checker)
                    subprocess.run(['mcopy','-o','-i',str(initrd),str(bad),'::/BOOT/FATFSCK.ELF'],check=True)
                    selection=work/'tampered-select.bin';selection.write_bytes(slot_record(1,0,(pair/'KERNEL.ELF').read_bytes(),initrd.read_bytes()))
                    replacements['INITRD.IMG']=initrd;replacements['SELECT.BIN']=selection
                    for name in ('INITRD.IMG','SELECT.BIN'):expectedHashes[name]=sha(replacements[name])
                for name in ('KERNEL.ELF','INITRD.IMG','SELECT.BIN'):
                    subprocess.run(['mcopy','-o','-i',media,str(replacements[name]),'::/RECOVERY/'+name],check=True)
                    embedded=subprocess.check_output(['mtype','-i',media,'::/RECOVERY/'+name])
                    if hashlib.sha256(embedded).hexdigest()!=expectedHashes[name]:
                        raise RuntimeError('embedded recovery pair mismatch: '+name)
            if scenario.startswith('slot-'):
                kernel=subprocess.check_output(['mtype','-i',media,'::/KERNEL/BROOK.ELF'])
                initrd=subprocess.check_output(['mtype','-i',media,'::/INITRD.IMG'])
                config=work/'cfg';config.write_bytes(subprocess.check_output(['mtype','-i',media,'::/BROOK.CFG'])+b'\nUPDATE_SLOTS=1\n')
                subprocess.run(['mcopy','-o','-i',media,str(config),'::/BROOK.CFG'],check=True)
                for directory in ('UPDATES','UPDATES/A','UPDATES/B'):
                    subprocess.run(['mmd','-i',media,'::/'+directory],check=True)
                for slot,name in ((0,'A'),(1,'B')):
                    k=work/('kernel-'+name);i=work/('initrd-'+name);s=work/('SELECT'+str(slot)+'.BIN')
                    k.write_bytes(kernel);i.write_bytes(initrd);s.write_bytes(slot_record(2-slot,slot,kernel,initrd))
                    for item,target in ((k,'::/UPDATES/'+name+'/KERNEL.ELF'),(i,'::/UPDATES/'+name+'/INITRD.IMG'),(s,'::/UPDATES/'+s.name)):
                        subprocess.run(['mcopy','-o','-i',media,str(item),target],check=True)
                # Clean boot can only succeed through a verified update slot.
                if scenario=='slot-clean':subprocess.run(['mdel','-i',media,'::/KERNEL/BROOK.ELF'],check=True)
            with disk.open('rb') as f:f.seek(133120*512+1024+104);fs_uuid=uuid.UUID(bytes=f.read(16))
            with partition.open('wb') as f:f.truncate(126976*512)
            subprocess.run(['mke2fs','-q','-F','-t','ext2','-b','4096','-I','128','-N','1024',
                            '-m','0','-O','none,filetype,sparse_super,large_file','-U',str(fs_uuid),str(partition)],
                           check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            with disk.open('r+b') as target,partition.open('rb') as data:
                target.seek(133120*512);shutil.copyfileobj(data,target,1024*1024)
            partition.unlink()
            with disk.open('r+b') as target:
                if scenario in ('dirty','slot-dirty','missing-recovery','corrupt-recovery','wrong-ata','checker-corrupt'):
                    target.seek(133120*512+1024+58);target.write(b'\0\0')
                if scenario=='wrong-uuid':target.seek(133120*512+1024+104);target.write(b'\xff')
            guard=bytearray(128);guard[:8]=b'BRKRCV01';struct.pack_into('<QI',guard,8,1,2 if scenario=='pending' else 1)
            guard[24:72]=bytes.fromhex(a.identity);struct.pack_into('<I',guard,124,zlib.crc32(guard[:124]))
            record=work/'guard.bin';record.write_bytes(guard)
            for name in ('GUARD0.BIN','GUARD1.BIN'):
                subprocess.run(['mcopy','-o','-i',media,str(record),'::/RECOVERY/'+name],check=True)
            if scenario=='missing-recovery':
                subprocess.run(['mdel','-i',media,'::/RECOVERY/INITRD.IMG'],check=True)
            if scenario=='corrupt-recovery':
                selection=bytearray(subprocess.check_output(['mtype','-i',media,'::/RECOVERY/SELECT.BIN']))
                selection[124]^=1;corrupt=work/'corrupt-select.bin';corrupt.write_bytes(selection)
                subprocess.run(['mcopy','-o','-i',media,str(corrupt),'::/RECOVERY/SELECT.BIN'],check=True)
            # Serial output from other CPUs can split the prefix from its ID.
            before=sha(disk);normal='AUDIO_CONTROL_FAIL';recovery='brook-ssd-recovery-manual-v1'
            isNormal=scenario in ('clean','slot-clean')
            broken=scenario in ('missing-recovery','corrupt-recovery')
            if pair:
                expect=('GUARD_RECORDS_MATCH' if scenario=='dirty' else
                        'GUARD_RECORDS_INCOMPLETE mismatch' if scenario=='pending' else
                        'GUARD_CHECK_INCOMPLETE load=-8' if scenario=='checker-corrupt' else 'SSD_GUARD_RESERVATION_FAILED')
            else:expect='Recovery required but payload invalid or missing.' if broken else normal if isNormal else recovery
            forbid=recovery if isNormal else normal
            # No F5 input: dirty/invalid data must reach verified recovery by itself.
            boot(disk,a.firmware,work,False,expect,90,True,forbid,sata_snapshot=not bool(pair),
                 hold_after_marker=2 if broken or pair else 0,
                 sata_serial=('BROOK-WRONG-DEVICE' if scenario=='wrong-ata' else 'BROOK-RECOVERY-QA') if pair else None)
            if sha(disk)!=before:raise RuntimeError('source fixture changed')
            if pair:
                serial=(work/'serial.log').read_text(errors='replace')
                lines=serial.splitlines()
                required=['Recovery required; normal boot blocked.','Brook SSD recovery payload verified']
                if scenario in ('dirty','pending','checker-corrupt'):required+=['SSD_GUARD_RESERVATION_READY']
                if scenario in ('dirty','pending'):required+=['GUARD_CHECK_RESULT exit=0']
                required+=[expect]
                if any(lines.count(marker)!=1 for marker in required) or [lines.index(marker) for marker in required]!=sorted(lines.index(marker) for marker in required):
                    raise RuntimeError('missing, repeated or reordered guard outcome')
                if scenario in ('wrong-uuid','wrong-ata') and any(x.startswith(('SSD_GUARD_RESERVATION_READY','GUARD_CHECK_','GUARD_RECORDS_')) for x in lines):
                    raise RuntimeError('rejected target unexpectedly admitted')
                if scenario=='checker-corrupt' and any(x.startswith(('GUARD_CHECK_RESULT','GUARD_RECORDS_')) for x in lines):
                    raise RuntimeError('tampered checker was not rejected after reservation')
                if scenario=='dirty' and any(x.startswith(('GUARD_CHECK_INCOMPLETE','GUARD_RECORDS_INCOMPLETE')) for x in lines):
                    raise RuntimeError('successful run has contradictory failure')
            shutil.copyfile(work/'serial.log',a.output/(scenario+'-serial.log'))
            print('RECOVERY_DATA_DISPATCH_'+scenario.upper().replace('-','_')+'_PASS',flush=True)


if __name__=='__main__':main()
