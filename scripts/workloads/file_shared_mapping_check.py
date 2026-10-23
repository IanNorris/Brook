#!/usr/bin/env python3
"""Disposable Brook VM: tmpfs/FileObject mappings, IPC, quota and RO mounts.

Run as root. The child owns a cloned private mount namespace. Requires the
adjacent shared_futex_check.py; no host mounts or Steam account data are used.
"""
import ctypes as c
import errno
import json
import mmap
import os
import signal
import tempfile
from pathlib import Path
from shared_futex_check import main as aliases

lib=c.CDLL(None,use_errno=True)
lib.syscall.restype=c.c_long
lib.syscall.argtypes=[c.c_long]*7
lib.mount.argtypes=[c.c_char_p,c.c_char_p,c.c_char_p,c.c_ulong,c.c_void_p]
lib.umount2.argtypes=[c.c_char_p,c.c_int]

def mount(source,target,kind,flags):
    def encoded(value):return value.encode() if value is not None else None
    assert lib.mount(encoded(source),encoded(target),encoded(kind),flags,None)==0,c.get_errno()

def denied(fn, error):
    try:fn()
    except OSError as failure:assert failure.errno==error,(failure.errno,error)
    else:raise AssertionError('operation unexpectedly accepted')

def coherence(directory):
    path=directory+'/coherence'
    fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
    os.ftruncate(fd,8192)
    a=mmap.mmap(fd,8192,flags=mmap.MAP_SHARED,prot=3)
    other=os.open(path,os.O_RDWR)
    b=mmap.mmap(other,8192,flags=mmap.MAP_SHARED,prot=3)
    a[:4]=b'ABCD';assert b[:4]==b'ABCD' and os.pread(other,4,0)==b'ABCD'
    os.pwrite(other,b'EFGH',4096);assert a[4096:4100]==b'EFGH'
    private=mmap.mmap(fd,8192,flags=mmap.MAP_PRIVATE,prot=3)
    private[:4]=b'priv';assert a[:4]==b'ABCD' and os.pread(fd,4,0)==b'ABCD'
    private.close()
    denied(lambda:os.ftruncate(fd,0),errno.EBUSY)
    denied(lambda:os.open(path,os.O_RDWR|os.O_TRUNC),errno.EBUSY)
    reader=os.open(path,os.O_RDONLY)
    denied(lambda:mmap.mmap(reader,8192,flags=mmap.MAP_SHARED,prot=3),errno.EACCES)
    os.close(reader)
    os.unlink(path)
    replacement=os.open(path,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
    os.ftruncate(replacement,8192)
    assert os.pread(replacement,4,0)==b'\0'*4
    a[:4]=b'OLD!';assert b[:4]==b'OLD!' and os.pread(replacement,4,0)==b'\0'*4
    a.flush();os.fsync(fd);os.close(fd);os.close(other)
    assert b[:4]==b'OLD!'
    a.close();b.close();os.close(replacement);os.unlink(path);os.sync()
    print(json.dumps(dict(passed=True,case='coherence-private-unlink-recreate',backing=directory.rsplit('/',1)[-1])),flush=True)

def quota(directory):
    path=directory+'/sparse';fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
    os.ftruncate(fd,16*1024*1024)
    view=mmap.mmap(fd,64*4096,flags=mmap.MAP_SHARED,prot=3)
    for i in range(64):view[i*4096]=i
    fill=directory+'/fill';other=os.open(fill,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
    total=0
    while True:
        try:n=os.write(other,b'\0'*65536)
        except OSError as e:
            assert e.errno==errno.ENOSPC;break
        assert n>0
        total+=n;assert total<=16*1024*1024
    before=os.statvfs(directory).f_bfree
    view[0]=0x5a;view.flush();os.fsync(fd)
    assert os.pread(fd,1,0)==b'Z'
    os.unlink(path);os.close(fd);view.close();os.sync()
    assert os.statvfs(directory).f_bfree>before,'unlinked mapping retained at full quota'
    os.unlink(fill);os.close(other);os.sync()
    print(json.dumps(dict(passed=True,case='quota-full-sparse-writeback-reclaim',fill_bytes=total)),flush=True)

def main():
    assert os.geteuid()==0,'run inside disposable Brook VM as root'
    signal.alarm(240)
    pid=lib.syscall(56,0x20000|17,0,0,0,0,0)
    assert pid>=0,c.get_errno()
    if pid:
        assert os.waitpid(pid,0)==(pid,0)
        print('FILE_SHARED_MAPPING_PASS',flush=True);return
    signal.alarm(240)
    mount(None,'/',None,0x44000)
    base=Path(tempfile.mkdtemp(prefix='file-mapping-',dir='/data/home'))
    ram=base/'tmpfs';disk=base/'ext2';ro=base/'readonly'
    for path in (ram,disk,ro):path.mkdir()
    mount('tmpfs',str(ram),'tmpfs',6)
    coherence(str(ram));coherence(str(disk))
    aliases(str(ram));aliases(str(disk))
    os.sync()
    path=ram/'readonly-value';path.write_bytes(b'A'*4096)
    mount(str(ram),str(ro),None,4096)
    mount(None,str(ro),None,4096|32|1|6)
    denied(lambda:os.open(ro/'readonly-value',os.O_RDWR),errno.EROFS)
    fd=os.open(ro/'readonly-value',os.O_RDONLY)
    denied(lambda:mmap.mmap(fd,4096,flags=mmap.MAP_SHARED,prot=3),errno.EACCES)
    private=mmap.mmap(fd,4096,flags=mmap.MAP_PRIVATE,prot=3);private[0]=ord('P')
    private.close();os.close(fd)
    assert lib.umount2(str(ro).encode(),2)==0,c.get_errno()
    path.unlink();os.sync()
    quota(str(ram))
    assert lib.umount2(str(ram).encode(),2)==0,c.get_errno()
    for path in (ram,disk,ro):path.rmdir()
    base.rmdir()

if __name__=='__main__':main()
