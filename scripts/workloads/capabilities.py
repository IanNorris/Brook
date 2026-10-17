#!/usr/bin/env python3
"""Empty-capability ABI probes in isolated children; parent IDs/caps unchanged.

Arguments: output JSON path, optional 'brook'. Linux runs nonroot self ABI
checks; Brook additionally checks its explicit root/foreign-query exclusions.
"""
import ctypes as c
import errno
import json
import os
from pathlib import Path
import sys
import threading

libc=c.CDLL(None,use_errno=True);libc.syscall.restype=c.c_long
brook=len(sys.argv)>2 and sys.argv[2]=='brook'
results={}


def raw(number,*args):
    c.set_errno(0)
    result=libc.syscall(c.c_long(number),*(c.c_void_p(v) for v in args))
    if result==-1:raise OSError(c.get_errno(),'syscall %d'%number)
    return result


def denied(expected,fn):
    try:fn()
    except OSError as exc:assert exc.errno==expected,(expected,exc.errno)
    else:raise AssertionError('expected errno %d'%expected)


def header(version=0x20080522,pid=0):return (c.c_uint32*2)(version,pid & 0xffffffff)
def ptr(value):return c.addressof(value)
def cap(number,h,data):return raw(number,ptr(h),data)


def child(name,fn,ids=None):
    if ids and os.geteuid()!=0:
        results[name]={'skipped':'requires root inside Brook'};return
    r,w=os.pipe();pid=os.fork()
    if pid==0:
        os.close(r)
        try:
            if os.geteuid()==0:
                os.setgroups([]);os.setresgid(100,100,100)
                os.setresuid(*(ids or (1000,1000,1000)))
            detail=fn()
            payload={'passed':True,'detail':detail}
        except BaseException as exc:
            payload={'passed':False,'error':repr(exc),'uids':os.getresuid()}
        data=json.dumps(payload).encode()
        while data:data=data[os.write(w,data):]
        os.close(w);os._exit(0)
    os.close(w);data=bytearray()
    while chunk:=os.read(r,65536):data.extend(chunk)
    os.close(r);_,status=os.waitpid(pid,0)
    assert status==0,(name,status)
    results[name]=json.loads(data)
    print('CAPABILITY_CASE',name,json.dumps(results[name]),flush=True)
    assert results[name]['passed'],(name,results[name])


def versions():
    for version,words in [(0x19980330,3),(0x20071026,6),(0x20080522,6)]:
        h=header(version)
        data=(c.c_uint32*8)(*[0xfaceb00c]*8)
        assert raw(125,ptr(h),ptr(data)+4)==0
        assert list(data)==[0xfaceb00c]+[0]*words+[0xfaceb00c]*(7-words),list(data)
        assert raw(126,ptr(h),ptr(data)+4)==0
    return 'V1/V2/V3 exact-size get/set with surrounding canaries'


def negotiation():
    for null in [False,True]:
        h=header(0);data=(c.c_uint32*6)(*[0xcafebabe]*6)
        fn=lambda:raw(125,ptr(h),0 if null else ptr(data))
        if null:assert fn()==0
        else:denied(errno.EINVAL,fn)
        assert h[0]==0x20080522 and list(data)==[0xcafebabe]*6
    h=header(0)
    denied(errno.EINVAL,lambda:raw(126,ptr(h),0))
    assert h[0]==0x20080522
    for version in [0x19980330,0x20071026,0x20080522]:
        assert cap(125,header(version,0xffffffff),0)==0
    return 'preferred-version negotiation and null-data query'


def reject_bits():
    h=header()
    for word in range(6):
        for bit in [1,0x80000000]:
            # Linux masks reserved bits above CAP_LAST_CAP. Brook's restricted
            # contract explicitly rejects every nonzero unsupported word.
            if not brook and word>=3 and bit==0x80000000:continue
            data=(c.c_uint32*6)();data[word]=bit
            try:denied(errno.EPERM,lambda:raw(126,ptr(h),ptr(data)))
            except AssertionError as exc:raise AssertionError(('grant-field',word,bit,str(exc)))
            assert data[word]==bit
    data=(c.c_uint32*6)()
    assert raw(126,ptr(h),ptr(data))==0
    try:os.setresuid(0,0,0)
    except OSError as exc:
        assert exc.errno in ((errno.EPERM,) if brook else (errno.EPERM,errno.EINVAL)),exc.errno
    else:raise AssertionError('empty capabilities regained root')
    assert os.getresuid()==(os.getuid(),)*3
    return 'all fields/upper words reject grants; empty clear cannot regain root'


def targets():
    zero=(c.c_uint32*6)()
    own=raw(186)
    for target in [0,own]:
        h=header(pid=target)
        assert raw(125,ptr(h),ptr(zero))==0
        assert raw(126,ptr(h),ptr(zero))==0
    def reject_target(fn):
        return denied(errno.EPERM,fn)
    reject_target(lambda:cap(126,header(pid=-1),ptr(zero)))
    denied(errno.EINVAL,lambda:cap(125,header(pid=-1),ptr(zero)))
    if brook:
        denied(errno.ENOSYS,lambda:cap(125,header(pid=999999),ptr(zero)))
        denied(errno.EPERM,lambda:cap(126,header(pid=999999),ptr(zero)))
    errors=[]
    def peer():
        try:
            tid=raw(186);h=header(pid=tid)
            assert tid!=os.getpid()
            assert raw(125,ptr(h),ptr(zero))==0
            assert raw(126,ptr(h),ptr(zero))==0
            if brook:denied(errno.ENOSYS,lambda:cap(125,header(pid=os.getpid()),ptr(zero)))
            reject_target(lambda:cap(126,header(pid=os.getpid()),ptr(zero)))
        except BaseException as exc:errors.append(repr(exc))
    t=threading.Thread(target=peer);t.start();t.join(5)
    assert not t.is_alive() and not errors,errors
    return 'self TID versus leader/foreign/negative PID, including a worker thread'


def buffers():
    h=header();data=(c.c_uint32*6)()
    for number in [125,126]:
        denied(errno.EFAULT,lambda:raw(number,0,ptr(data)))
    denied(errno.EFAULT,lambda:raw(126,ptr(h),0))
    for number in [125,126]:denied(errno.EFAULT,lambda:raw(number,ptr(h),1))
    libc.mmap.restype=c.c_void_p
    address=libc.mmap(None,8192,3,0x22,-1,0)
    assert address and address!=c.c_void_p(-1).value
    try:
        assert libc.mprotect(c.c_void_p(address+4096),4096,0)==0
        # Header's version is readable, PID is not. Null-data get reads no PID.
        c.c_uint32.from_address(address+4092).value=0x20080522
        assert raw(125,address+4092,0)==0
        denied(errno.EFAULT,lambda:raw(125,address+4092,ptr(data)))
        denied(errno.EFAULT,lambda:raw(126,address+4092,ptr(data)))
        # Twelve-byte V1 buffer ends at the page; V3 must not overrun it.
        tail=(c.c_uint32*3).from_address(address+4084)
        assert cap(125,header(0x19980330),address+4084)==0
        assert list(tail)==[0]*3
        assert cap(126,header(0x19980330),address+4084)==0
        tail[:]=[7,8,9]
        for number in [125,126]:denied(errno.EFAULT,lambda:raw(number,ptr(h),address+4084))
        # Linux copy_to_user may change the accessible prefix before EFAULT.
        # Brook validates the whole range before its copy.
        if brook:assert list(tail)==[7,8,9]
        c.c_uint32.from_address(address).value=0
        assert libc.mprotect(c.c_void_p(address),4096,1)==0
        denied(errno.EFAULT,lambda:raw(125,address,0))
    finally:
        libc.munmap(c.c_void_p(address),8192)
    return 'inaccessible/null headers/data, page boundaries, readonly version negotiation'


def root_unsupported():
    h=header();data=(c.c_uint32*6)(*[0xbeef]*6)
    for number in [125,126]:
        denied(errno.ENOSYS,lambda:raw(number,ptr(h),ptr(data)))
    assert list(data)==[0xbeef]*6
    return {'uids':os.getresuid(),'contract':'root-capable tuple remains unsupported'}


try:
    child('versions',versions)
    child('negotiation',negotiation)
    child('reject-grants',reject_bits)
    child('targets',targets)
    child('buffers',buffers)
    if brook:
        for name,ids in [('root',(0,0,0)),('real-root',(0,1000,1000)),
                         ('effective-root',(1000,0,1000)),('saved-root',(1000,1000,0))]:
            child(name,root_unsupported,ids)
finally:
    if len(sys.argv)>1:Path(sys.argv[1]).write_text(json.dumps(results,indent=2)+'\n')
    print('CAPABILITY_RESULT',json.dumps(results),flush=True)
