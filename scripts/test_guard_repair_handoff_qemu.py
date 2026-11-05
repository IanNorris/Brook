#!/usr/bin/env python3
"""Exercise automatic repair, reset and guarded desktop on a disposable QEMU disk."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from hardware_update_protocol import record
from test_recovery_menu_boot import boot
from test_recovery_data_dispatch import metadata_hash
from test_ext2_idle_flush_sata_qemu import DIRS,FILES


def check_pair(path,identity,automatic):
    manifest=json.loads((path/'manifest.json').read_text())
    if manifest.get('profile')!='qemu-fixture' or manifest.get('guard_identity')!=identity:
        raise ValueError('pair is not the identity-bound QEMU fixture')
    flags=manifest.get('kernel_build_flags',{})
    if flags.get('BROOK_SSD_AUTO_RECOVERY','OFF')!=('ON' if automatic else 'OFF') or flags.get('BROOK_SSD_GUARD_PENDING_TEST','OFF')!=('OFF' if automatic else 'ON'):
        raise ValueError('unexpected automatic/test kernel configuration')
    for name in ('KERNEL.ELF','INITRD.IMG','SELECT.BIN'):
        item=path/name;meta=manifest['files'][name]
        if item.is_symlink() or item.stat().st_size!=meta['bytes'] or hashlib.sha256(item.read_bytes()).hexdigest()!=meta['sha256']:
            raise ValueError('pair manifest mismatch: '+name)


def compact(source,dest,tree):
    tree.mkdir()
    subprocess.run(['mcopy','-s','-i',str(source),'::*',str(tree)],check=True)
    with dest.open('wb') as f:f.truncate(8*1024*1024)
    subprocess.run(['mformat','-i',str(dest),'::'],check=True)
    subprocess.run(['mcopy','-s','-i',str(dest),*[str(p) for p in tree.iterdir()],'::/'],check=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','efi','firmware','desktop-pair','recovery-pair','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--identity',required=True)
    a=parser.parse_args()
    if len(a.identity)!=96 or any(c not in '0123456789abcdef' for c in a.identity):parser.error('invalid fixture identity')
    if a.image.is_symlink() or a.image.stat().st_size!=128*1024*1024:parser.error('expected disposable 128 MiB fixture')
    check_pair(a.desktop_pair,a.identity,False);check_pair(a.recovery_pair,a.identity,True)
    a.output.mkdir(parents=True,exist_ok=False)
    with tempfile.TemporaryDirectory(prefix='brk-auto-handoff-') as tmp:
        work=Path(tmp);disk=work/'disk.raw';shutil.copyfile(a.image,disk);media=str(disk)+'@@1048576'
        normal=work/'normal.img';recovery=work/'recovery.img'
        compact(a.desktop_pair/'INITRD.IMG',normal,work/'normal-tree')
        compact(a.recovery_pair/'INITRD.IMG',recovery,work/'recovery-tree')
        service=work/'GDSVC.ELF';script=work/'INIT.RC';defaults=work/'defaults.ini'
        subprocess.run(['cc','-nostdlib','-static','-fno-stack-protector','-no-pie','-O2',
                        str(ROOT/'scripts/workloads/guard_handoff_service.c'),'-o',str(service)],check=True)
        script.write_text('set wm\nset vfb none\nsched eevdf\nkconsole\nrun --service --once --console --uid 1000 /boot/GDSVC.ELF\n')
        defaults.write_text('default-value\n')
        for src,target in ((service,'::/BOOT/GDSVC.ELF'),(script,'::/BOOT/INIT.RC'),(defaults,'::/ETC/desktop-defaults.ini')):
            subprocess.run(['mcopy','-o','-i',str(normal),str(src),target],check=True)
        selection=work/'select.bin';selection.write_bytes(record(1,0,(a.recovery_pair/'KERNEL.ELF').read_bytes(),recovery.read_bytes()))
        for src,target in ((a.efi,'::/EFI/BOOT/BOOTX64.EFI'),(a.desktop_pair/'KERNEL.ELF','::/KERNEL/BROOK.ELF'),
                           (normal,'::/INITRD.IMG'),(a.recovery_pair/'KERNEL.ELF','::/RECOVERY/KERNEL.ELF'),
                           (recovery,'::/RECOVERY/INITRD.IMG'),(selection,'::/RECOVERY/SELECT.BIN')):
            subprocess.run(['mcopy','-o','-i',media,str(src),target],check=True)
        partition=work/'data.img'
        with disk.open('rb') as f:f.seek(133120*512+1024+104);fs_uuid=uuid.UUID(bytes=f.read(16))
        with partition.open('wb') as f:f.truncate(126976*512)
        subprocess.run(['mke2fs','-q','-F','-t','ext2','-b','4096','-I','128','-N','1024','-m','0',
                        '-O','none,filetype,sparse_super,large_file','-U',str(fs_uuid),str(partition)],check=True,stderr=subprocess.DEVNULL)
        witness=work/'placeholder';witness.write_text('synthetic fixture\n')
        for name in DIRS:
            for command in (f'mkdir /{name}',f'set_inode_field /{name} uid 1000',f'set_inode_field /{name} gid 1000'):
                subprocess.run(['debugfs','-w','-R',command,str(partition)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        for name in FILES:
            subprocess.run(['debugfs','-w','-R',f'write {witness} /{name}',str(partition)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            if name.startswith(('bin/','profile/bin/')):
                subprocess.run(['debugfs','-w','-R',f'set_inode_field /{name} mode 0100755',str(partition)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        with disk.open('r+b') as dst,partition.open('rb') as src:
            dst.seek(133120*512);shutil.copyfileobj(src,dst);dst.seek(133120*512+1024+58);dst.write(b'\0\0')
        guard=bytearray(128);guard[:8]=b'BRKRCV01';struct.pack_into('<QI',guard,8,1,1)
        guard[24:72]=bytes.fromhex(a.identity);struct.pack_into('<I',guard,124,zlib.crc32(guard[:124]))
        guard_file=work/'guard.bin';guard_file.write_bytes(guard)
        for name in ('GUARD0.BIN','GUARD1.BIN'):
            subprocess.run(['mcopy','-o','-i',media,str(guard_file),'::/RECOVERY/'+name],check=True)
        journal=work/'PANIC.BIN';journal.write_bytes(bytes(258*512))
        subprocess.run(['mcopy','-o','-i',media,str(journal),'::/PANIC.BIN'],check=True)
        metadata=metadata_hash(disk)
        first=work/'repair';first.mkdir()
        boot(disk,a.firmware,first,False,'GUARD_COMPLETE_RESTART',120,True,sata_snapshot=False,
             sata_serial='BROOK-RECOVERY-QA',expect_reset=True,reset_evidence_marker='GUARD_COMPLETE_RESTART',
             failure_marker='GUARD_EXT2_INCOMPLETE',extra_fw_cfg=['name=opt/brook/power,string=q35-reset'])
        shutil.copyfile(first/'serial.log',a.output/'repair-serial.log')
        serial=(first/'serial.log').read_text(errors='replace')
        for marker in ('Recovery required; normal boot blocked.','brook-ssd-recovery-automatic-v1','GUARD_AUTO_NEEDS_CHECK','GUARD_EXT2_VERIFIED_PENDING','GUARD_COMPLETE_RESULT outcome=2'):
            if marker not in serial:raise RuntimeError('automatic repair did not reach '+marker)
        guards=[subprocess.check_output(['mtype','-i',media,'::/RECOVERY/'+name]) for name in ('GUARD0.BIN','GUARD1.BIN')]
        if guards[0]!=guards[1] or len(guards[0])!=128 or struct.unpack_from('<I',guards[0],16)[0]!=1 or struct.unpack_from('<I',guards[0],20)[0]!=0 or zlib.crc32(guards[0][:124])!=struct.unpack_from('<I',guards[0],124)[0]:
            raise RuntimeError('repair did not publish matching valid clean guards')
        if metadata_hash(disk)!=metadata:raise RuntimeError('repair changed GPT or disk metadata')
        with disk.open('rb') as src,partition.open('wb') as dst:src.seek(133120*512);dst.write(src.read(126976*512))
        with partition.open('rb') as f:
            f.seek(1024+58)
            if f.read(2)!=b'\x01\x00':raise RuntimeError('repair did not clean ext2')
        subprocess.run(['e2fsck','-f','-n',str(partition)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        second=work/'desktop';second.mkdir()
        boot(disk,a.firmware,second,False,'GUARD_JOURNAL_NORMAL_SCRIPT status=0',120,True,sata_snapshot=False,
             hold_after_marker=5,sata_serial='BROOK-RECOVERY-QA',failure_marker='GUARD_JOURNAL_DESKTOP_FAIL',
             extra_fw_cfg=['name=opt/guard-journal-pin,string=test-b','name=opt/guard-panic-capture,string=test-1'])
        shutil.copyfile(second/'serial.log',a.output/'desktop-serial.log')
        serial=(second/'serial.log').read_text(errors='replace')
        if ('GUARD_DESKTOP_SERVICE_PASS' not in serial or 'brook-ssd-recovery-automatic-v1' in serial or
            'Recovery required; normal boot blocked.' in serial):
            raise RuntimeError('guarded desktop service did not start after repair')
        if subprocess.check_output(['mtype','-i',media,'::/PANIC.BIN'])!=journal.read_bytes():raise RuntimeError('journal unexpectedly changed')
        for index in range(2):
            if subprocess.check_output(['mtype','-i',media,f'::/RECOVERY/GUARD{index}.BIN'])!=guards[index]:
                raise RuntimeError('desktop changed verified clean guard records')
        if metadata_hash(disk)!=metadata:raise RuntimeError('desktop changed GPT or disk metadata')
        with disk.open('rb') as src,partition.open('wb') as dst:src.seek(133120*512);dst.write(src.read(126976*512))
        subprocess.run(['e2fsck','-f','-n',str(partition)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        content=subprocess.check_output(['debugfs','-R','cat /home/brook/SERVICE.TXT',str(partition)],stderr=subprocess.DEVNULL)
        if content!=b'guarded desktop service\n':raise RuntimeError('desktop service data missing')
        fat=work/'esp.img'
        with disk.open('rb') as src,fat.open('wb') as dst:src.seek(2048*512);dst.write(src.read(131072*512))
        subprocess.run(['fsck.fat','-n',str(fat)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        print('GUARD_REPAIR_TO_DESKTOP_PASS')

if __name__=='__main__':main()
