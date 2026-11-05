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
from test_panic_fat_qemu import verify_record
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from hardware_update_protocol import record as slot_record


def sha(path):
    with path.open('rb') as source:return hashlib.file_digest(source,'sha256').hexdigest()


def outside_esp_hash(path):
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        digest.update(stream.read(2048*512));stream.seek(133120*512)
        for block in iter(lambda:stream.read(1024*1024),b''):digest.update(block)
    return digest.hexdigest()

def metadata_hash(path):
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        digest.update(stream.read(2048*512));stream.seek(260096*512)
        for block in iter(lambda:stream.read(1024*1024),b''):digest.update(block)
    return digest.hexdigest()


def crosslink_root_files(disk,first,source_name,target_name):
    """Make target's first cluster equal source's on a private FAT fixture."""
    with disk.open('r+b') as stream:
        stream.seek(first*512);boot=stream.read(512)
        if len(boot)!=512 or struct.unpack_from('<H',boot,11)[0]!=512:raise RuntimeError('invalid FAT geometry')
        reserved=struct.unpack_from('<H',boot,14)[0];fats=boot[16];roots=struct.unpack_from('<H',boot,17)[0]
        fat=struct.unpack_from('<H',boot,22)[0] or struct.unpack_from('<I',boot,36)[0]
        cluster_size=boot[13];data=reserved+fats*fat+(roots*32+511)//512
        if roots:
            regions=[((reserved+fats*fat)*512,roots*32)]
        else:
            cluster=struct.unpack_from('<I',boot,44)[0];regions=[];seen=set()
            while 2<=cluster<0x0ffffff8:
                if cluster in seen or len(seen)>65536:raise RuntimeError('invalid root chain')
                seen.add(cluster);regions.append(((data+(cluster-2)*cluster_size)*512,cluster_size*512))
                stream.seek((first+reserved)*512+cluster*4);raw=stream.read(4)
                if len(raw)!=4:raise RuntimeError('short FAT read')
                cluster=struct.unpack('<I',raw)[0]&0x0fffffff
        found={}
        for offset,size in regions:
            stream.seek(first*512+offset);entries=stream.read(size)
            for i in range(0,len(entries)-31,32):
                name=entries[i:i+11]
                if name in (source_name,target_name):found[name]=(first*512+offset+i,entries[i:i+32])
        if len(found)!=2:raise RuntimeError('expected root files not found')
        src=found[source_name][1];dst_offset,dst=found[target_name];entry=bytearray(dst)
        def first_cluster(record):
            return struct.unpack_from('<H',record,26)[0] | struct.unpack_from('<H',record,20)[0]<<16
        if min(first_cluster(src),first_cluster(dst))<2 or first_cluster(src)==first_cluster(dst):
            raise RuntimeError('fixture files lack distinct allocated clusters')
        for off in (20,26):entry[off:off+2]=src[off:off+2]
        stream.seek(dst_offset);stream.write(entry)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','efi','firmware','output'):p.add_argument('--'+name,required=True,type=Path)
    p.add_argument('--identity',required=True)
    p.add_argument('--recovery-pair',type=Path,help='Exact prepared read-only guard pair for end-to-end testing')
    p.add_argument('--case',help='Run one named scenario')
    a=p.parse_args();source=a.image.resolve(strict=True)
    if a.image.is_symlink() or not source.is_file() or source.stat().st_size!=128*1024*1024:
        p.error('expected the disposable 128 MiB fixture')
    if len(a.identity)!=96 or any(c not in '0123456789abcdef' for c in a.identity):p.error('invalid identity')
    a.output.mkdir(parents=True,exist_ok=False)
    scenarios=('dirty','pending','wrong-uuid','wrong-ata','checker-corrupt','journal-crosslink',
               'journal-pin','journal-pin-existing','journal-pin-short','journal-pin-corrupt','journal-pin-crosslink','journal-publish','journal-publish-existing','journal-publish-panic','journal-publish-unarmed','journal-publish-failed','journal-desktop','journal-desktop-dirty','journal-desktop-panic','journal-desktop-failed','journal-desktop-shutdown','journal-desktop-identity-denied','journal-desktop-script') if a.recovery_pair else ('clean','dirty','wrong-uuid','pending','slot-clean','slot-dirty','missing-recovery','corrupt-recovery')
    if a.case:
        if a.case not in scenarios:p.error('invalid scenario for this mode')
        scenarios=(a.case,)
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
                elif scenario=='journal-desktop-script':
                    initrd=work/'desktop-script-initrd.img';shutil.copyfile(pair/'INITRD.IMG',initrd)
                    service=work/'GDSVC.ELF';script=work/'INIT.RC'
                    service_source=Path(__file__).resolve().parent/'workloads/guard_handoff_service.c'
                    subprocess.run(['cc','-nostdlib','-static','-fno-stack-protector','-no-pie','-O2',str(service_source),'-o',str(service)],check=True)
                    script.write_text('set wm\nset vfb none\nsched eevdf\nkconsole\n'
                                      'run --service --once --console --uid 1000 /boot/GDSVC.ELF\n')
                    for file,name in ((service,'GDSVC.ELF'),(script,'INIT.RC')):
                        subprocess.run(['mcopy','-o','-i',str(initrd),str(file),'::/BOOT/'+name],check=True)
                    selection=work/'desktop-script-select.bin';selection.write_bytes(slot_record(1,0,(pair/'KERNEL.ELF').read_bytes(),initrd.read_bytes()))
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
            if scenario in ('journal-desktop','journal-desktop-dirty','journal-desktop-panic','journal-desktop-failed','journal-desktop-shutdown','journal-desktop-script'):
                from test_ext2_idle_flush_sata_qemu import DIRS,FILES
                witness=work/'fixture-file';witness.write_text('synthetic fixture\n')
                for name in DIRS:
                    for command in (f'mkdir /{name}',f'set_inode_field /{name} uid 1000',f'set_inode_field /{name} gid 1000'):
                        subprocess.run(['debugfs','-w','-R',command,str(partition)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                for name in FILES:
                    if scenario=='journal-desktop-failed' and name=='profile/manifest.tsv':continue
                    subprocess.run(['debugfs','-w','-R',f'write {witness} /{name}',str(partition)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                    if name.startswith(('bin/','profile/bin/')):
                        subprocess.run(['debugfs','-w','-R',f'set_inode_field /{name} mode 0100755',str(partition)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            with disk.open('r+b') as target,partition.open('rb') as data:
                target.seek(133120*512);shutil.copyfileobj(data,target,1024*1024)
            partition.unlink()
            with disk.open('r+b') as target:
                if scenario in ('dirty','slot-dirty','missing-recovery','corrupt-recovery','wrong-ata','checker-corrupt','journal-crosslink','journal-desktop-dirty') or scenario.startswith(('journal-pin','journal-publish')):
                    target.seek(133120*512+1024+58);target.write(b'\0\0')
                if scenario=='wrong-uuid':target.seek(133120*512+1024+104);target.write(b'\xff')
            guard=bytearray(128);guard[:8]=b'BRKRCV01';struct.pack_into('<QI',guard,8,1,2 if scenario=='pending' else 1)
            guard[24:72]=bytes.fromhex(a.identity);struct.pack_into('<I',guard,124,zlib.crc32(guard[:124]))
            record=work/'guard.bin';record.write_bytes(guard)
            for name in ('GUARD0.BIN','GUARD1.BIN'):
                subprocess.run(['mcopy','-o','-i',media,str(record),'::/RECOVERY/'+name],check=True)
            if scenario in ('journal-crosslink','journal-desktop','journal-desktop-dirty','journal-desktop-panic','journal-desktop-failed','journal-desktop-shutdown','journal-desktop-identity-denied','journal-desktop-script') or scenario.startswith(('journal-pin','journal-publish')):
                contents=bytearray((257 if scenario=='journal-pin-short' else 258)*512)
                if scenario=='journal-pin-corrupt':contents[0]=0xff
                if scenario in ('journal-pin-existing','journal-publish-existing','journal-publish-panic'):
                    payload=b'previous panic evidence';contents[:8]=b'BRKPAN01'
                    struct.pack_into('<IIQI',contents,8,1,len(payload),7,zlib.crc32(payload))
                    struct.pack_into('<I',contents,28,zlib.crc32(contents[:512]));contents[512:512+len(payload)]=payload
                journal=work/'panic.bin';journal.write_bytes(contents)
                subprocess.run(['mcopy','-i',media,str(journal),'::/PANIC.BIN'],check=True)
                if scenario in ('journal-crosslink','journal-pin-crosslink'):
                    other=work/'other.bin';other.write_bytes(bytes(512))
                    subprocess.run(['mcopy','-i',media,str(other),'::/OTHER.BIN'],check=True)
                    crosslink_root_files(disk,2048,b'PANIC   BIN',b'OTHER   BIN')
            if scenario=='missing-recovery':
                subprocess.run(['mdel','-i',media,'::/RECOVERY/INITRD.IMG'],check=True)
            if scenario=='corrupt-recovery':
                selection=bytearray(subprocess.check_output(['mtype','-i',media,'::/RECOVERY/SELECT.BIN']))
                selection[124]^=1;corrupt=work/'corrupt-select.bin';corrupt.write_bytes(selection)
                subprocess.run(['mcopy','-o','-i',media,str(corrupt),'::/RECOVERY/SELECT.BIN'],check=True)
            # Serial output from other CPUs can split the prefix from its ID.
            before=sha(disk);outside=outside_esp_hash(disk) if scenario.startswith('journal-publish') else None
            metadata_before=metadata_hash(disk) if scenario in ('journal-desktop','journal-desktop-panic','journal-desktop-failed','journal-desktop-shutdown','journal-desktop-script') else None
            with disk.open('rb') as f:
                f.seek(133120*512);first_data_before=f.read(512)
            normal='AUDIO_CONTROL_FAIL';recovery='brook-ssd-recovery-manual-v1'
            isNormal=scenario in ('clean','slot-clean')
            broken=scenario in ('missing-recovery','corrupt-recovery')
            if pair:
                expect=('GUARD_RECORDS_MATCH' if scenario=='dirty' else
                        'GUARD_RECORDS_INCOMPLETE mismatch' if scenario=='pending' else
                        'GUARD_JOURNAL_NORMAL_SCRIPT status=0' if scenario=='journal-desktop-script' else
                        'GUARD_JOURNAL_DESKTOP_IDENTITY_DENIED forged=0 retry=0 publication=0' if scenario=='journal-desktop-identity-denied' else
                        'shutdown: request=dddddddddddddddd0000000000000001 phase=ready result=0' if scenario=='journal-desktop-shutdown' else
                        'GUARD_JOURNAL_DESKTOP_FAILED mounted=0 write=-30' if scenario=='journal-desktop-failed' else
                        'GUARD_JOURNAL_DESKTOP_PASS' if scenario=='journal-desktop' else
                        'PANIC_TEST_DISK_CAPTURE_OK' if scenario=='journal-desktop-panic' else
                        'GUARD_JOURNAL_DESKTOP_FAIL arm=0' if scenario=='journal-desktop-dirty' else
                        'GUARD_CHECK_INCOMPLETE load=-8' if scenario=='checker-corrupt' else
                        'GUARD_CHECK_RESULT exit=1' if scenario in ('journal-crosslink','journal-pin-crosslink') else
                        'PANIC_TEST_DISK_CAPTURE_OK' if scenario=='journal-publish-panic' else
                        'GUARD_JOURNAL_PANIC_DENIED status=-13' if scenario=='journal-publish-unarmed' else
                        'GUARD_JOURNAL_PUBLISH_FAILED status=-2 retry=-22 arm=-22' if scenario=='journal-publish-failed' else
                        'GUARD_JOURNAL_PUBLISH_RESULT status=0' if scenario.startswith('journal-publish') else
                        'GUARD_JOURNAL_PIN_RESULT status=0 repeated=-22' if scenario in ('journal-pin','journal-pin-existing') else
                        'GUARD_JOURNAL_PIN_RESULT status=-22 repeated=-22' if scenario in ('journal-pin-short','journal-pin-corrupt') else 'SSD_GUARD_RESERVATION_FAILED')
            else:expect='Recovery required but payload invalid or missing.' if broken else normal if isNormal else recovery
            forbid=recovery if isNormal else normal
            # No F5 input: dirty/invalid data must reach verified recovery by itself.
            boot(disk,a.firmware,work,scenario in ('journal-desktop','journal-desktop-dirty','journal-desktop-panic','journal-desktop-failed','journal-desktop-shutdown','journal-desktop-identity-denied','journal-desktop-script'),expect,90,True,forbid,sata_snapshot=not bool(pair),
                 hold_after_marker=5 if scenario=='journal-desktop-script' else 2 if broken or pair else 0,
                 sata_serial=('BROOK-WRONG-DEVICE' if scenario=='wrong-ata' else 'BROOK-RECOVERY-QA') if pair else None,
                 failure_marker=('GUARD_JOURNAL_DESKTOP_FAIL' if scenario in ('journal-desktop','journal-desktop-panic','journal-desktop-shutdown','journal-desktop-script') else
                                 'GUARD_JOURNAL_PUBLISH_RESULT status=-' if scenario.startswith('journal-publish') else
                                 'SSD_GUARD_RESERVATION_FAILED' if scenario.startswith('journal-pin') else None),
                 extra_fw_cfg=(['name=opt/guard-journal-pin,string=test-7','name=opt/guard-panic-capture,string=test-1','name=opt/brook/power,string=q35-reset','name=opt/brook/panic-test-reset,string=test-1s','name=opt/brook/panic-test-headless,string=yes'] if scenario=='journal-desktop-panic' else
                               ['name=opt/guard-journal-pin,string=test-b','name=opt/guard-panic-capture,string=test-1'] if scenario=='journal-desktop-script' else
                               ['name=opt/guard-journal-pin,string=test-a'] if scenario=='journal-desktop-identity-denied' else
                               ['name=opt/guard-journal-pin,string=test-9','name=opt/guard-panic-capture,string=test-1'] if scenario=='journal-desktop-shutdown' else
                               ['name=opt/guard-journal-pin,string=test-8','name=opt/guard-panic-capture,string=test-1'] if scenario=='journal-desktop-failed' else
                               ['name=opt/guard-journal-pin,string=test-6','name=opt/guard-panic-capture,string=test-1'] if scenario in ('journal-desktop','journal-desktop-dirty') else
                               ['name=opt/guard-journal-pin,string=test-3','name=opt/guard-panic-capture,string=test-1','name=opt/brook/power,string=q35-reset','name=opt/brook/panic-test-reset,string=test-1s','name=opt/brook/panic-test-headless,string=yes'] if scenario=='journal-publish-panic' else
                               ['name=opt/guard-journal-pin,string=test-4'] if scenario=='journal-publish-unarmed' else
                               ['name=opt/guard-journal-pin,string=test-5'] if scenario=='journal-publish-failed' else
                               ['name=opt/guard-journal-pin,string=test-2' if scenario.startswith('journal-publish') else 'name=opt/guard-journal-pin,string=test-1'] if scenario.startswith(('journal-pin','journal-publish')) else None),
                 expect_reset=scenario in ('journal-publish-panic','journal-desktop-panic'))
            shutil.copyfile(work/'serial.log',a.output/(scenario+'-serial.log'))
            if scenario in ('journal-desktop','journal-desktop-panic','journal-desktop-failed','journal-desktop-shutdown','journal-desktop-script'):
                if scenario=='journal-desktop-failed':
                    with disk.open('rb') as f:
                        f.seek(133120*512)
                        if f.read(512)!=first_data_before:raise RuntimeError('failed desktop adapter changed data sector')
                journal_after=subprocess.check_output(['mtype','-i',media,'::/PANIC.BIN'])
                if scenario=='journal-desktop-panic':
                    serial=(work/'serial.log').read_text(errors='replace')
                    packet=next((line[10:] for line in serial.splitlines() if line.startswith('PANIC_HEX:')),None)
                    if not packet:raise RuntimeError('desktop panic packet missing')
                    verify_record(journal_after,0,1,bytes.fromhex(packet))
                elif journal_after!=contents:raise RuntimeError('journal changed during desktop handoff')
                if metadata_hash(disk)!=metadata_before:raise RuntimeError('GPT or disk metadata changed during handoff')
                for index in range(2):
                    if subprocess.check_output(['mtype','-i',media,f'::/RECOVERY/GUARD{index}.BIN'])!=guard:raise RuntimeError('guard changed during desktop handoff')
                fat=work/'desktop-fat.img'
                with disk.open('rb') as source,fat.open('wb') as target:
                    source.seek(2048*512);target.write(source.read(131072*512))
                subprocess.run(['fsck.fat','-n',str(fat)],check=True,stdout=subprocess.DEVNULL,timeout=30)
                with disk.open('rb') as source,partition.open('wb') as target:
                    source.seek(133120*512);target.write(source.read(126976*512))
                if scenario=='journal-desktop-shutdown':
                    with partition.open('rb') as f:
                        f.seek(1024+58)
                        if f.read(2)!=b'\x01\x00':raise RuntimeError('checked shutdown did not mark ext2 clean')
                result=subprocess.run(['e2fsck','-f','-n',str(partition)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
                if result.returncode:raise RuntimeError('desktop filesystem check failed: '+result.stdout.decode(errors='replace')[-2000:])
                if scenario!='journal-desktop-failed':
                    content=subprocess.check_output(['debugfs','-R','cat /home/brook/GUARD.TXT',str(partition)],stderr=subprocess.DEVNULL)
                    if content!=b'guarded desktop handoff\n':raise RuntimeError('desktop witness mismatch')
                if scenario=='journal-desktop-script':
                    content=subprocess.check_output(['debugfs','-R','cat /home/brook/SERVICE.TXT',str(partition)],stderr=subprocess.DEVNULL)
                    if content!=b'guarded desktop service\n':raise RuntimeError('desktop service witness mismatch')
                if scenario=='journal-desktop-panic':
                    reboot=work/'desktop-reboot';reboot.mkdir()
                    boot(disk,a.firmware,reboot,False,'GUARD_JOURNAL_PIN_RESULT status=0 repeated=-22',90,True,forbid,
                         sata_snapshot=False,hold_after_marker=2,sata_serial='BROOK-RECOVERY-QA',
                         failure_marker='GUARD_JOURNAL_PIN_RESULT status=-',extra_fw_cfg=['name=opt/guard-journal-pin,string=test-1'])
                    if subprocess.check_output(['mtype','-i',media,'::/PANIC.BIN'])!=journal_after:raise RuntimeError('journal changed after desktop panic reboot')
                    shutil.copyfile(reboot/'serial.log',a.output/'journal-desktop-panic-reboot-serial.log')
                if scenario=='journal-desktop-shutdown':
                    reboot=work/'desktop-reboot';reboot.mkdir()
                    boot(disk,a.firmware,reboot,True,'GUARD_JOURNAL_DESKTOP_PASS',90,True,forbid,
                         sata_snapshot=False,hold_after_marker=2,sata_serial='BROOK-RECOVERY-QA',
                         failure_marker='GUARD_JOURNAL_DESKTOP_FAIL',
                         extra_fw_cfg=['name=opt/guard-journal-pin,string=test-6','name=opt/guard-panic-capture,string=test-1'])
                    if subprocess.check_output(['mtype','-i',media,'::/PANIC.BIN'])!=journal_after:raise RuntimeError('journal changed after checked shutdown reboot')
                    shutil.copyfile(reboot/'serial.log',a.output/'journal-desktop-shutdown-reboot-serial.log')
            elif scenario.startswith('journal-publish'):
                if outside_esp_hash(disk)!=outside:raise RuntimeError('write outside the ESP')
                current=subprocess.check_output(['mtype','-i',media,'::/PANIC.BIN'])
                if scenario=='journal-publish-panic':
                    serial=(work/'serial.log').read_text(errors='replace')
                    packet=next((line[10:] for line in serial.splitlines() if line.startswith('PANIC_HEX:')),None)
                    if not packet:raise RuntimeError('panic packet absent from serial')
                    if current[:129*512]!=contents[:129*512]:raise RuntimeError('previous panic record changed')
                    verify_record(current,1,8,bytes.fromhex(packet))
                elif current!=contents:raise RuntimeError('published journal changed')
                if scenario!='journal-publish-failed' and subprocess.check_output(['mtype','-i',media,'::/PINTST.TXT'])!=b'checked journal remains pinned\n':raise RuntimeError('ordinary FAT witness differs')
                fat=work/'fat.img'
                with disk.open('rb') as inp, fat.open('wb') as out:
                    inp.seek(2048*512);remaining=(133120-2048)*512
                    while remaining:
                        block=inp.read(min(1024*1024,remaining))
                        if not block:raise RuntimeError('short FAT partition read')
                        out.write(block);remaining-=len(block)
                subprocess.run(['fsck.fat','-n',str(fat)],check=True,stdout=subprocess.DEVNULL,timeout=30)
                if scenario=='journal-publish-panic':
                    reboot=work/'reboot';reboot.mkdir()
                    boot(disk,a.firmware,reboot,False,'GUARD_JOURNAL_PIN_RESULT status=0 repeated=-22',90,True,forbid,
                         sata_snapshot=False,hold_after_marker=2,sata_serial='BROOK-RECOVERY-QA',
                         failure_marker='GUARD_JOURNAL_PIN_RESULT status=-',
                         extra_fw_cfg=['name=opt/guard-journal-pin,string=test-1'])
                    if subprocess.check_output(['mtype','-i',media,'::/PANIC.BIN'])!=current:
                        raise RuntimeError('panic journal changed on the subsequent boot')
                    reboot_serial=(reboot/'serial.log').read_text(errors='replace')
                    if 'GUARD_JOURNAL_PIN_RESULT status=0 repeated=-22' not in reboot_serial:
                        raise RuntimeError('subsequent boot did not read and pin the captured journal')
                    shutil.copyfile(reboot/'serial.log',a.output/'journal-publish-panic-reboot-serial.log')
            elif sha(disk)!=before:raise RuntimeError('source fixture changed')
            if pair:
                serial=(work/'serial.log').read_text(errors='replace')
                lines=serial.splitlines()
                required=([] if scenario in ('journal-desktop','journal-desktop-dirty','journal-desktop-panic','journal-desktop-failed','journal-desktop-shutdown','journal-desktop-identity-denied','journal-desktop-script') else ['Recovery required; normal boot blocked.'])+['Brook SSD recovery payload verified']
                if scenario in ('dirty','pending','checker-corrupt','journal-crosslink') or scenario.startswith(('journal-pin','journal-publish')):required+=['SSD_GUARD_RESERVATION_READY']
                if scenario in ('dirty','pending','journal-pin','journal-pin-existing','journal-pin-short','journal-pin-corrupt') or scenario.startswith('journal-publish'):required+=['GUARD_CHECK_RESULT exit=0']
                if scenario.startswith('journal-publish'):required+=['GUARD_JOURNAL_PIN_RESULT status=0 repeated=-22','GUARD_JOURNAL_PUBLISH_EARLY_ARM status=-22']
                if scenario in ('journal-desktop','journal-desktop-dirty','journal-desktop-panic','journal-desktop-failed','journal-desktop-shutdown','journal-desktop-identity-denied','journal-desktop-script'):required+=['SSD_GUARD_RESERVATION_READY','GUARD_CHECK_RESULT exit=0','GUARD_JOURNAL_PIN_RESULT status=0 repeated=-22','GUARD_JOURNAL_PUBLISH_EARLY_ARM status=-22']
                if scenario in ('journal-desktop','journal-desktop-panic','journal-desktop-failed','journal-desktop-shutdown','journal-desktop-script'):required+=['GUARD_JOURNAL_DESKTOP_SYSTEM_IDENTITY status=0']
                if scenario in ('journal-desktop-panic','journal-desktop-shutdown','journal-desktop-script'):required+=['GUARD_JOURNAL_DESKTOP_PASS']
                if scenario in ('journal-desktop','journal-desktop-panic','journal-desktop-shutdown','journal-desktop-script'):
                    required.insert(required.index('GUARD_JOURNAL_DESKTOP_PASS') if scenario in ('journal-desktop-panic','journal-desktop-shutdown','journal-desktop-script') else len(required),
                                    'GUARD_JOURNAL_DESKTOP_LEGACY status=0 stat=0 open=0 read=2')
                if scenario=='journal-desktop-shutdown':required+=['GUARD_JOURNAL_DESKTOP_SHUTDOWN state=closing error=0']
                if scenario.startswith('journal-publish') and scenario!='journal-publish-failed':required+=['GUARD_JOURNAL_PUBLISH_UNLINK status=-16','GUARD_JOURNAL_PUBLISH_REMOVE open=-16 closed=0','GUARD_JOURNAL_PUBLISH_RENAME status=-16','GUARD_JOURNAL_PUBLISH_MOVE open=-16 journal_target=-16 closed=0']
                if scenario=='journal-publish-panic':required+=['GUARD_JOURNAL_PUBLISH_RESULT status=0','GUARD_JOURNAL_PANIC_ARM status=0']
                if scenario=='journal-desktop-script' and 'GUARD_DESKTOP_SERVICE_PASS' not in serial:
                    raise RuntimeError('startup service did not run its executable')
                required+=[expect]
                if any(lines.count(marker)!=1 for marker in required) or [lines.index(marker) for marker in required]!=sorted(lines.index(marker) for marker in required):
                    raise RuntimeError('missing, repeated or reordered guard outcome')
                if scenario in ('wrong-uuid','wrong-ata') and any(x.startswith(('SSD_GUARD_RESERVATION_READY','GUARD_CHECK_','GUARD_RECORDS_')) for x in lines):
                    raise RuntimeError('rejected target unexpectedly admitted')
                if scenario=='checker-corrupt' and any(x.startswith(('GUARD_CHECK_RESULT','GUARD_RECORDS_')) for x in lines):
                    raise RuntimeError('tampered checker was not rejected after reservation')
                if scenario in ('journal-crosslink','journal-pin-crosslink') and any(x.startswith(('GUARD_RECORDS_','GUARD_JOURNAL_PIN_RESULT')) for x in lines):
                    raise RuntimeError('crosslinked journal unexpectedly reached record admission')
                if scenario in ('journal-crosslink','journal-pin-crosslink') and not all(x in serial.lower() for x in ('/panic.bin','/other.bin','share clusters')):
                    raise RuntimeError('checker did not identify the injected journal crosslink')
                if scenario=='dirty' and any(x.startswith(('GUARD_CHECK_INCOMPLETE','GUARD_RECORDS_INCOMPLETE')) for x in lines):
                    raise RuntimeError('successful run has contradictory failure')
            shutil.copyfile(work/'serial.log',a.output/(scenario+'-serial.log'))
            print('RECOVERY_DATA_DISPATCH_'+scenario.upper().replace('-','_')+'_PASS',flush=True)


if __name__=='__main__':main()
