#!/usr/bin/env python3
"""Deterministic disposable namespace/data oracle for ext2 write-behind."""
import hashlib
import json
import os
from pathlib import Path
import sys
import time

root=Path(sys.argv[1]);root.mkdir()  # refuse reuse
native=Path('/proc/ext2probe').exists()

def probe(name):
    path=Path('/proc')/name
    if not path.exists():return []
    lines=path.read_text().splitlines();keys=lines[0].split()
    return [dict(zip(keys,map(int,line.split()))) for line in lines[1:]]

def total(rows,key):return sum(row[key] for row in rows)

def put(fd,data,offset):
    while data:
        n=os.pwrite(fd,data,offset);assert n>0;data=data[n:];offset+=n

expected={}
for i in range(32):
    path=root/f'file-{i:02d}.tmp';data=bytearray((i*17+j*13+(j>>8))&255 for j in range(65536+i*17))
    fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
    try:
        put(fd,data,0)
        for j in range(8):
            patch=bytes(((i+j+k*7)&255) for k in range(127+j*53))
            at=(i*4093+j*211)%(len(data)-len(patch))
            put(fd,patch,at);data[at:at+len(patch)]=patch
        if i%5==0:
            os.ftruncate(fd,len(data)//2);del data[len(data)//2:]
        assert os.pread(fd,len(data),0)==data
    finally:os.close(fd)
    target=root/f'file-{i:02d}.bin';os.rename(path,target)
    expected[target.name]=bytes(data)
    if i%7==0:
        alias=root/f'link-{i:02d}.bin';os.link(target,alias)
        assert alias.read_bytes()==data;expected[alias.name]=bytes(data)
os.symlink('file-01.bin',root/'relative-link');assert (root/'relative-link').read_bytes()==expected['file-01.bin']
# O_SYNC must reach the filesystem sync hook, not merely admit queued bytes.
a=probe('ext2probe');fd=os.open(root/'sync.bin',os.O_CREAT|os.O_EXCL|os.O_WRONLY|os.O_SYNC,0o600)
try:assert os.write(fd,b'O_SYNC durability witness\n'*64)==len(b'O_SYNC durability witness\n'*64)
finally:os.close(fd)
b=probe('ext2probe')
if a:assert total(b,'fsync')>total(a,'fsync')
expected['sync.bin']=b'O_SYNC durability witness\n'*64
if native:os.sync()
# A copied, unsynced write must progress through the age worker. The existing
# mmap worker may race one attempt; retry with a different phase, same bytes.
a=probe('ext2writeback');enabled=any(row['capacity'] for row in a)
background_seen=False
for attempt in range(3 if enabled else 1):
    witness=b'background writeback witness\n'*1024
    (root/'background.bin').write_bytes(witness)
    if not enabled:
        if native:time.sleep(2.25)
        break
    time.sleep(2.25)
    b=probe('ext2writeback')
    if total(b,'backgroundDrains')>total(a,'backgroundDrains'):
        background_seen=True;break
if enabled:assert background_seen,'no age-worker progress after three attempts'
expected['background.bin']=witness
for name,data in expected.items():assert (root/name).read_bytes()==data
fd=os.open(root,os.O_RDONLY|os.O_DIRECTORY)
try:os.fsync(fd)
finally:os.close(fd)
if native:os.sync()
manifest={name:hashlib.sha256(data).hexdigest() for name,data in sorted(expected.items())}
print(json.dumps(dict(passed=True,files=manifest,background_enabled=enabled,background_progress=background_seen),sort_keys=True))
