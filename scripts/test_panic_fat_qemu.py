#!/usr/bin/env python3
"""Test panic capture into a retained FAT file on a disposable SATA disk.

This validates the integrated mapping and raw I/O on a private, clean fixture.
It does not establish crosslink exclusion for a production filesystem or SSD
power-loss durability. The required kernel is BROOK_AHCI_PANIC_TEST=ON.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import time
import uuid
import zlib

SECTOR=512
JOURNAL_SECTORS=258
SLOT_SECTORS=129


def file_sectors(disk, first, path):
    media=f'{disk}@@{first*SECTOR}'
    with disk.open('rb') as stream:
        stream.seek(first*SECTOR)
        boot=stream.read(SECTOR)
    bps=struct.unpack_from('<H',boot,11)[0]
    per_cluster=boot[13]
    reserved=struct.unpack_from('<H',boot,14)[0]
    fats=boot[16]
    roots=struct.unpack_from('<H',boot,17)[0]
    fat_size=struct.unpack_from('<H',boot,22)[0] or struct.unpack_from('<I',boot,36)[0]
    if bps!=SECTOR or not per_cluster or not fat_size:
        raise RuntimeError('unexpected FAT geometry')
    data_start=first+reserved+fats*fat_size+(roots*32+511)//512
    text=subprocess.check_output(['mshowfat','-i',media,path],text=True)
    clusters=[]
    for start,end in re.findall(r'<(\d+)(?:-(\d+))?>',text):
        a=int(start);b=int(end) if end else a
        if b<a:raise RuntimeError('invalid FAT extent')
        clusters.extend(range(a,b+1))
    mapped=[data_start+(cluster-2)*per_cluster+offset for cluster in clusters for offset in range(per_cluster)]
    if len(mapped)<JOURNAL_SECTORS or len(set(mapped[:JOURNAL_SECTORS]))!=JOURNAL_SECTORS:
        raise RuntimeError('invalid independent journal map')
    return mapped[:JOURNAL_SECTORS]


def fragment_test_file(disk, first, path):
    """Reorder one private FAT32 chain; all payload sectors are zero."""
    media=f'{disk}@@{first*SECTOR}'
    text=subprocess.check_output(['mshowfat','-i',media,path],text=True)
    clusters=[]
    for start,end in re.findall(r'<(\d+)(?:-(\d+))?>',text):
        a=int(start);b=int(end) if end else a
        clusters.extend(range(a,b+1))
    with disk.open('r+b') as stream:
        stream.seek(first*SECTOR);boot=stream.read(SECTOR)
        bps=struct.unpack_from('<H',boot,11)[0];per_cluster=boot[13]
        reserved=struct.unpack_from('<H',boot,14)[0];fats=boot[16]
        root_entries=struct.unpack_from('<H',boot,17)[0]
        fat_size=struct.unpack_from('<I',boot,36)[0]
        if bps!=512 or per_cluster!=1 or root_entries or fats!=2 or len(clusters)!=JOURNAL_SECTORS:
            raise RuntimeError('unexpected private FAT32 geometry for fragmentation')
        order=clusters[:1]
        for i in range(1,len(clusters)-1,2):order.extend((clusters[i+1],clusters[i]))
        if len(order)<len(clusters):order.append(clusters[-1])
        for copy in range(fats):
            base=(first+reserved+copy*fat_size)*SECTOR
            for index,cluster in enumerate(order):
                stream.seek(base+cluster*4);raw=stream.read(4)
                if len(raw)!=4:raise RuntimeError('short FAT entry')
                old=struct.unpack('<I',raw)[0]
                value=order[index+1] if index+1<len(order) else 0x0fffffff
                stream.seek(base+cluster*4);stream.write(struct.pack('<I',(old&0xf0000000)|value))
    after=subprocess.check_output(['mshowfat','-i',media,path],text=True)
    if len(re.findall(r'<',after))<2:raise RuntimeError('fixture did not fragment')


def outside_digest(path, excluded):
    excluded=set(excluded);digest=hashlib.sha256()
    with path.open('rb') as stream:
        i=0
        while data:=stream.read(1024*1024):
            if len(data)%SECTOR:raise RuntimeError('unaligned disk image')
            for offset in range(0,len(data),SECTOR):
                if i not in excluded:digest.update(data[offset:offset+SECTOR])
                i+=1
    return digest.digest()


def verify_record(contents,slot,sequence,expected):
    start=slot*SLOT_SECTORS*SECTOR
    header=bytearray(contents[start:start+SECTOR])
    if header[:8]!=b'BRKPAN01':raise RuntimeError('missing journal header')
    version,length,seq,crc,header_crc=struct.unpack_from('<IIQII',header,8)
    struct.pack_into('<I',header,28,0)
    packet=contents[start+SECTOR:start+SECTOR+length]
    if version!=1 or seq!=sequence or zlib.crc32(header)!=header_crc or zlib.crc32(packet)!=crc or packet!=expected:
        raise RuntimeError('journal packet or checksum mismatch')


def run_case(work,image,kernel,firmware,short,timeout,fragmented=False,exporter=None,normal_kernel=None):
    boot=work/'boot.raw';sata=work/'sata.raw'
    shutil.copyfile(image,boot);shutil.copyfile(image,sata)
    table=json.loads(subprocess.check_output(['sfdisk','--json',str(sata)]))['partitiontable']
    if table['label']!='gpt' or sata.stat().st_size!=262144*SECTOR:
        raise RuntimeError('expected the 128 MiB GPT fixture')
    part=table['partitions'][0];first=part['start']
    if first!=2048:raise RuntimeError('unexpected ESP offset')
    target=f'{sata}@@{first*SECTOR}'
    journal=work/'PANIC.BIN'
    with journal.open('wb') as stream:stream.truncate((JOURNAL_SECTORS-(1 if short else 0))*SECTOR)
    subprocess.run(['mcopy','-i',target,str(journal),'::/PANIC.BIN'],check=True)
    if fragmented:fragment_test_file(sata,first,'::/PANIC.BIN')
    sectors=[] if short else file_sectors(sata,first,'::/PANIC.BIN')
    before=outside_digest(sata,sectors)
    boot_esp=str(boot)+'@@1048576';initrd=work/'initrd.img'
    subprocess.run(['mcopy','-i',boot_esp,'::/INITRD.IMG',str(initrd)],check=True)
    for name in ('MEDIA','MEDIA/SYSTEM','DATA'):
        subprocess.run(['mmd','-i',str(initrd),'::/'+name],check=True)
    records={'ahci-observe':'1\n','ahci-read-only':'1\n',
             'ahci-write-target':'BRKAHCI1\n32902,10530,0,31,2,0,262144,512,512\nQEMU HARDDISK\nBROOK-PANIC-FAT\n',
             'ahci-boot-volume':f'BRKESP1\n{uuid.UUID(table["id"]).bytes_le.hex()}\n'
                                f'{uuid.UUID(part["uuid"]).bytes_le.hex()}\n{first},{part["size"]}\n'}
    for name,content in records.items():
        item=work/name;item.write_text(content)
        subprocess.run(['mcopy','-o','-i',str(initrd),str(item),'::/ETC/'+name],check=True)
    init=work/'INIT.RC';init.write_text('panic fat_capture_test\n')
    subprocess.run(['mcopy','-o','-i',str(initrd),str(init),'::/BOOT/INIT.RC'],check=True)
    subprocess.run(['mcopy','-o','-i',boot_esp,str(initrd),'::/INITRD.IMG'],check=True)
    subprocess.run(['mcopy','-o','-i',boot_esp,str(kernel),'::/KERNEL/BROOK.ELF'],check=True)
    variables=work/'vars.fd';shutil.copyfile(firmware/'OVMF_VARS.fd',variables)
    serial=work/'serial.log'
    args=['qemu-system-x86_64','-machine','q35','-accel','tcg','-cpu','max','-smp','2','-m','1024','-nodefaults',
          '-drive',f'if=pflash,format=raw,readonly=on,file={firmware / "OVMF_CODE.fd"}',
          '-drive',f'if=pflash,format=raw,file={variables}','-device','VGA',
          '-device','qemu-xhci,id=usbc','-drive',f'if=none,id=boot,format=raw,file={boot}',
          '-device','usb-storage,bus=usbc.0,port=1,drive=boot,bootindex=1',
          '-drive',f'if=none,id=sata,format=raw,file={sata}',
          '-device','ide-hd,drive=sata,bus=ide.0,serial=BROOK-PANIC-FAT',
          '-display','none','-serial',f'file:{serial}','-no-reboot',
          '-fw_cfg','name=opt/brook/power,string=q35-reset',
          '-fw_cfg','name=opt/brook/panic-test-reset,string=test-1s',
          '-fw_cfg','name=opt/brook/panic-test-disk,string=journal-fat-1']
    old_slot=None;latest_packet=None
    for boot_number in range(1 if short else 2):
        serial.unlink(missing_ok=True)
        with (work/f'qemu-{boot_number}.log').open('wb') as errors:
            proc=subprocess.Popen(args,stdout=subprocess.DEVNULL,stderr=errors)
            try:
                try:status=proc.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    output=serial.read_text(errors='replace') if serial.exists() else ''
                    raise RuntimeError('timed out: '+output[-5000:])
            finally:
                if proc.poll() is None:proc.kill();proc.wait()
        output=serial.read_text(errors='replace') if serial.exists() else ''
        if status!=0 or 'PANIC_TEST_RESET_WAIT' not in output:raise RuntimeError(output[-5000:])
        if short:
            if 'PANIC_TEST_FAT_REJECTED' not in output or 'PANIC_TEST_DISK_CAPTURE_OK' in output:
                raise RuntimeError('short journal was not rejected: '+output[-5000:])
        else:
            if 'PANIC_TEST_FAT_READY' not in output or 'PANIC_TEST_DISK_CAPTURE_OK' not in output:
                raise RuntimeError('FAT capture not confirmed: '+output[-5000:])
            line=next((x for x in output.splitlines() if x.startswith('PANIC_HEX:')),None)
            if not line:raise RuntimeError('serial packet absent')
            latest_packet=bytes.fromhex(line[len('PANIC_HEX:'):])
            contents=subprocess.check_output(['mtype','-i',target,'::/PANIC.BIN'])
            verify_record(contents,boot_number,boot_number+1,latest_packet)
            if boot_number==0:old_slot=contents[:SLOT_SECTORS*SECTOR]
            elif contents[:SLOT_SECTORS*SECTOR]!=old_slot:raise RuntimeError('previous slot changed')
        if outside_digest(sata,sectors)!=before:raise RuntimeError('write outside journal sectors')
    if exporter and not short:
        # Reboot a regular kernel, without the panic writer's exclusive file
        # lease, and export through the normal FAT/VFS path as a guest job.
        bridge_id='a'*32
        init.write_text('run --console --uid 0 /boot/PANICLOG.ELF /media/system/PANIC.BIN /tmp/panic-packet.bin\n'
                        'run --console --uid 0 /boot/PANICLOG.ELF --bridge '+bridge_id+'\n')
        subprocess.run(['mcopy','-o','-i',str(initrd),str(init),'::/BOOT/INIT.RC'],check=True)
        subprocess.run(['mcopy','-o','-i',str(initrd),str(exporter),'::/BOOT/PANICLOG.ELF'],check=True)
        subprocess.run(['mcopy','-o','-i',boot_esp,str(initrd),'::/INITRD.IMG'],check=True)
        subprocess.run(['mcopy','-o','-i',boot_esp,str(normal_kernel),'::/KERNEL/BROOK.ELF'],check=True)
        serial.unlink(missing_ok=True)
        with (work/'qemu-export.log').open('wb') as errors:
            proc=subprocess.Popen(args,stdout=subprocess.DEVNULL,stderr=errors)
            try:
                deadline=time.monotonic()+timeout
                while time.monotonic()<deadline:
                    output=serial.read_text(errors='replace') if serial.exists() else ''
                    if output.count('PANICLOG: sequence=')>=2:break
                    if 'PANICLOG: mkdir:' in output or 'PANICLOG: unsafe artifact directory' in output:
                        raise RuntimeError('bridge export failed: '+output[-4000:])
                    if proc.poll() is not None:raise RuntimeError('export guest exited: '+output[-4000:])
                    time.sleep(.1)
                else:raise RuntimeError('export guest timed out: '+output[-4000:])
            finally:
                if proc.poll() is None:proc.terminate();proc.wait(timeout=10)
        expected=f'PANICLOG: sequence=2 bytes={len(latest_packet)} crc32={zlib.crc32(latest_packet):08x} output=/tmp/panic-packet.bin'
        if expected not in output:raise RuntimeError('guest export mismatch: '+output[-4000:])
        expected_bridge=f'PANICLOG: sequence=2 bytes={len(latest_packet)} crc32={zlib.crc32(latest_packet):08x} output={bridge_id}'
        if expected_bridge not in output:raise RuntimeError('guest bridge export mismatch: '+output[-4000:])
        if subprocess.check_output(['mtype','-i',target,'::/PANIC.BIN'])!=contents:
            raise RuntimeError('guest export changed the journal')
    partition=work/'esp.img'
    with sata.open('rb') as source,partition.open('wb') as output:
        source.seek(first*SECTOR);remaining=part['size']*SECTOR
        while remaining:
            data=source.read(min(1024*1024,remaining))
            if not data:raise RuntimeError('short partition read')
            output.write(data);remaining-=len(data)
    subprocess.run(['fsck.fat','-n',str(partition)],check=True,timeout=30)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True)
    parser.add_argument('--kernel',type=Path,required=True)
    parser.add_argument('--firmware',type=Path,required=True)
    parser.add_argument('--timeout',type=int,default=120)
    parser.add_argument('--case',choices=['capture','fragmented','short-file'])
    parser.add_argument('--exporter',type=Path,help='Static PANICLOG.ELF to exercise after captures')
    parser.add_argument('--normal-kernel',type=Path,help='Regular kernel for the export boot')
    args=parser.parse_args()
    if bool(args.exporter)!=bool(args.normal_kernel):parser.error('exporter and normal kernel are required together')
    image,kernel,firmware=(p.resolve(strict=True) for p in (args.image,args.kernel,args.firmware))
    with tempfile.TemporaryDirectory(prefix='brook-panic-fat-') as temp:
        for name,short in [('capture',False),('fragmented',False),('short-file',True)]:
            if args.case and args.case!=name:continue
            work=Path(temp)/name;work.mkdir();run_case(work,image,kernel,firmware,short,args.timeout,fragmented=name=='fragmented',
                exporter=args.exporter.resolve(strict=True) if args.exporter else None,
                normal_kernel=args.normal_kernel.resolve(strict=True) if args.normal_kernel else None)
            print(name+': PASS',flush=True)


if __name__=='__main__':main()
