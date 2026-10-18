#!/usr/bin/env python3
"""Linux/Brook timerfd ABI regression. No absolute performance pass threshold.

Covers selected-clock timing, readiness, accounting, shared descriptions,
rearming blocked readers and fault-aware user buffers. Does not verify ET,
wall-clock adjustment/cancellation or close-vs-wait cancellation semantics.
"""
import argparse
import ctypes as c
import errno
import json
import os
import select
import struct
import threading
import time

class Timespec(c.Structure):
    _fields_ = [('sec', c.c_long), ('ns', c.c_long)]
class Itimerspec(c.Structure):
    _fields_ = [('interval', Timespec), ('value', Timespec)]
lib = c.CDLL(None, use_errno=True)
for name, args, result in [
    ('timerfd_create', [c.c_int,c.c_int],c.c_int),
    ('timerfd_settime',[c.c_int,c.c_int,c.c_void_p,c.c_void_p],c.c_int),
    ('timerfd_gettime',[c.c_int,c.c_void_p],c.c_int),
    ('read',[c.c_int,c.c_void_p,c.c_size_t],c.c_ssize_t),
    ('mmap',[c.c_void_p,c.c_size_t,c.c_int,c.c_int,c.c_int,c.c_long],c.c_void_p),
    ('mprotect',[c.c_void_p,c.c_size_t,c.c_int],c.c_int),
    ('munmap',[c.c_void_p,c.c_size_t],c.c_int),
]:
    f=getattr(lib,name);f.argtypes=args;f.restype=result
checks=[]
def check(name, condition):
    assert condition, name
    checks.append(name)
def ok(result):
    if result < 0: raise OSError(c.get_errno(),os.strerror(c.get_errno()))
    return result
def error(name,result,number):
    check(name,result == -1 and c.get_errno()==number)
def spec(value=0,interval=0):
    return Itimerspec(Timespec(*divmod(interval,10**9)),Timespec(*divmod(value,10**9)))
def arm(fd,value=0,interval=0,flags=0,old=None):
    v=spec(value,interval);ok(lib.timerfd_settime(fd,flags,c.byref(v),c.byref(old) if old is not None else None))
def get(fd):
    v=Itimerspec();ok(lib.timerfd_gettime(fd,c.byref(v)));return (v.value.sec*10**9+v.value.ns,v.interval.sec*10**9+v.interval.ns)
def read(fd):return struct.unpack('=Q',os.read(fd,8))[0]
def empty(fd,name):
    value=c.c_uint64();error(name,lib.read(fd,c.byref(value),8),errno.EAGAIN)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expect-fault-retains-count",action="store_true")
    args=parser.parse_args()
    error('invalid-clock',lib.timerfd_create(99,0),errno.EINVAL)
    error('invalid-create-flags',lib.timerfd_create(1,4),errno.EINVAL)
    error('get-invalid-fd',lib.timerfd_gettime(-1,1),errno.EBADF)
    wrong=os.open('/dev/null',os.O_RDONLY)
    try:error('get-wrong-fd',lib.timerfd_gettime(wrong,1),errno.EINVAL)
    finally:os.close(wrong)
    for clock in (1,0):
        fd=ok(lib.timerfd_create(clock,os.O_NONBLOCK|os.O_CLOEXEC))
        try:
            for absolute in (False,True):
                for method in ('poll','select','epoll','nested'):
                    started=time.clock_gettime_ns(clock)
                    arm(fd,started+16000000 if absolute else 16000000,flags=int(absolute))
                    ep=outer=None
                    try:
                        if method=='poll':
                            poll=select.poll();poll.register(fd,select.POLLIN);ready=poll.poll(2000)
                        elif method=='select':ready=select.select([fd],[],[],2)[0]
                        else:
                            ep=select.epoll();ep.register(fd,select.EPOLLIN)
                            if method=='nested':
                                outer=select.epoll();outer.register(ep.fileno(),select.EPOLLIN);ready=outer.poll(2)
                            else:ready=ep.poll(2)
                        check(f'ready-{clock}-{absolute}-{method}',bool(ready))
                        check(f'not-early-{clock}-{absolute}-{method}',time.clock_gettime_ns(clock)-started>=16000000)
                        check(f'read-{clock}-{absolute}-{method}',read(fd)==1)
                        empty(fd,f'consumed-{clock}-{absolute}-{method}')
                    finally:
                        if outer is not None:outer.close()
                        if ep is not None:ep.close()
            arm(fd,time.clock_gettime_ns(clock)-1,flags=1)
            check(f'past-ready-{clock}',bool(select.select([fd],[],[],2)[0]))
            check(f'past-absolute-{clock}',read(fd)==1)
            arm_before=time.clock_gettime_ns(clock)
            arm(fd,1000000,1000000)
            arm_after=time.clock_gettime_ns(clock);time.sleep(.035)
            value,interval=get(fd);check(f'periodic-get-{clock}',0<value<=1000000 and interval==1000000)
            get(fd);before_read=time.clock_gettime_ns(clock);count=read(fd);after_read=time.clock_gettime_ns(clock)
            low=(before_read-arm_after)//1000000
            high=(after_read-arm_before)//1000000
            check(f'periodic-accounting-{clock}:count={count},bounds={low}..{high}',low<=count<=high and count>0)
            arm(fd,0,1234567);check(f'disarmed-interval-{clock}',get(fd)==(0,1234567))
            old=Itimerspec();arm(fd,100000000,old=old)
            check(f'old-disarmed-interval-{clock}',old.interval.ns==1234567 and old.value.sec==old.value.ns==0)
            arm(fd,0);empty(fd,f'disarm-clears-{clock}')
            # Stage input before writing aliased old_value.
            v=spec(16000000);ok(lib.timerfd_settime(fd,0,c.byref(v),c.byref(v)))
            check(f'aliased-new-old-{clock}',get(fd)[0]>0 and v.value.sec==v.value.ns==0)
            arm(fd,0)
        finally:os.close(fd)
    fd=ok(lib.timerfd_create(1,os.O_NONBLOCK))
    try:
        v=spec(1)
        error('bad-set-flags',lib.timerfd_settime(fd,4,c.byref(v),None),errno.EINVAL)
        error('bad-new-pointer',lib.timerfd_settime(fd,0,1,None),errno.EFAULT)
        error('bad-old-pointer',lib.timerfd_settime(fd,0,c.byref(v),1),errno.EFAULT)
        error('bad-get-pointer',lib.timerfd_gettime(fd,1),errno.EFAULT)
        v=Itimerspec(Timespec(0,0),Timespec(-1,0))
        error('negative-seconds',lib.timerfd_settime(fd,0,c.byref(v),None),errno.EINVAL)
        v=Itimerspec(Timespec(0,0),Timespec(0,10**9))
        error('invalid-nanoseconds',lib.timerfd_settime(fd,0,c.byref(v),None),errno.EINVAL)
        page=os.sysconf('SC_PAGE_SIZE');mapping=lib.mmap(None,page*2,3,0x22,-1,0)
        check('fault-mapping',mapping not in (None,c.c_void_p(-1).value))
        try:
            ok(lib.mprotect(mapping+page,page,0))
            error('get-cross-page',lib.timerfd_gettime(fd,mapping+page-16),errno.EFAULT)
            error('set-cross-page',lib.timerfd_settime(fd,0,mapping+page-16,None),errno.EFAULT)
            arm(fd,time.monotonic_ns()-1,flags=1)
            check('read-fault-ready',bool(select.select([fd],[],[],2)[0]))
            result=lib.read(fd,mapping+page-4,8)
            if args.expect_fault_retains_count:
                error('read-cross-page',result,errno.EFAULT)
            else:
                check('read-cross-page-host',result==4 or result==-1 and c.get_errno()==errno.EFAULT)
            if args.expect_fault_retains_count:
                check('failed-read-retains-count',read(fd)==1)
            else:
                # Linux can consume a count before a failing put_user.
                try:read(fd)
                except BlockingIOError:pass
        finally:ok(lib.munmap(mapping,page*2))
        arm(fd,time.monotonic_ns()-1,flags=1)
        check('dup-ready',bool(select.select([fd],[],[],2)[0]))
        duplicate=os.dup(fd)
        try:check('dup-shared-count',read(duplicate)==1);empty(fd,'dup-consumed-original')
        finally:os.close(duplicate)
        arm(fd,time.monotonic_ns()-1,flags=1)
        check('fork-ready',bool(select.select([fd],[],[],2)[0]))
        pid=os.fork()
        if pid==0:
            try:os._exit(0 if read(fd)==1 else 2)
            except BaseException:os._exit(3)
        _,status=os.waitpid(pid,0);check('fork-shared-read',status==0);empty(fd,'fork-consumed-parent')
    finally:os.close(fd)
    # No waiter pointer can be overwritten by another blocking reader.
    fd=ok(lib.timerfd_create(1,0));values=[];failures=[]
    def reader():
        try:values.append(read(fd))
        except BaseException as e:failures.append(repr(e))
    try:
        workers=[threading.Thread(target=reader,daemon=True) for _ in range(2)]
        for t in workers:t.start()
        time.sleep(.025);check('disarmed-read-blocks',not values)
        arm(fd,16000000,16000000)
        for t in workers:t.join(2)
        check('rearm-wakes-both-readers',len(values)==2 and all(x>=1 for x in values) and not failures)
        arm(fd,10**9);values.clear()
        t=threading.Thread(target=reader,daemon=True);t.start();time.sleep(.025)
        start=time.monotonic();arm(fd,16000000);t.join(2)
        check('earlier-rearm',values==[1] and time.monotonic()-start<.8)
    finally:os.close(fd)
    print(json.dumps({'uid':os.geteuid(),'checks':checks,'count':len(checks)}),flush=True)
    print('TIMERFD_REGRESSION_PASS',flush=True)
if __name__=='__main__':main()
