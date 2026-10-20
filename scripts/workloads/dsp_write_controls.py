#!/usr/bin/env python3
"""Bounded guest DSP refill/control checks; use an isolated null-audio VM.

Owns all descriptors, mappings and writer threads. Fault injection uses only
this process's anonymous mapping or an intentionally invalid user pointer.
"""
import array
import ctypes as c
import errno
import fcntl
import json
import mmap
import os
import signal
import threading
import time


libc=c.CDLL(None,use_errno=True)
libc.write.argtypes=[c.c_int,c.c_void_p,c.c_size_t];libc.write.restype=c.c_ssize_t
libc.mprotect.argtypes=[c.c_void_p,c.c_size_t,c.c_int];libc.mprotect.restype=c.c_int
libc.syscall.restype=c.c_long
signal.signal(signal.SIGUSR1,lambda *_:None)
signal.siginterrupt(signal.SIGUSR1,True)


def integer(fd,cmd,value):
    arg=array.array('i',[value]);fcntl.ioctl(fd,cmd,arg,True);return arg[0]


def space(fd):
    arg=array.array('i',[0,0,0,0]);fcntl.ioctl(fd,0x8010500C,arg,True);return arg[3]


def open_paused():
    fd=os.open('/dev/dsp',os.O_WRONLY)
    for cmd,value in [(0xC0045005,16),(0xC0045006,2),(0xC0045002,44100),(0x40045010,0)]:
        integer(fd,cmd,value)
    return fd


def interrupt(writer):
    rc=libc.syscall(c.c_long(234),c.c_long(os.getpid()),c.c_long(writer.thread.native_id),c.c_long(signal.SIGUSR1))
    if rc:raise OSError(c.get_errno(),'tgkill')


class Writer:
    def __init__(self,fd,size):
        self.memory=mmap.mmap(-1,size);self.memory[:]=b'\x21'*size
        self.address=c.addressof(c.c_char.from_buffer(self.memory));self.size=size
        self.result=None;self.started=threading.Event()
        def run():
            c.set_errno(0);self.started.set()
            result=libc.write(fd,self.address,size);self.result=(result,c.get_errno())
        self.thread=threading.Thread(target=run,daemon=True);self.thread.start()
        assert self.started.wait(2)
    def join(self):
        self.thread.join(6);assert not self.thread.is_alive(),'writer did not complete'
        return self.result
    def close(self):
        assert not self.thread.is_alive();self.memory.close()


def wait_full(fd,writer):
    end=time.monotonic()+6
    while space(fd)!=0:
        assert writer.thread.is_alive(),writer.result
        assert time.monotonic()<end,'available prefix was never submitted'
        time.sleep(.01)
    assert writer.thread.is_alive(),'blocking write returned before its full chunk'


def control_case(action):
    fd=open_paused();writers=[];protected=None
    try:
        capacity=space(fd);assert capacity==65536,capacity
        if action=='fault-before':
            c.set_errno(0);rc=libc.write(fd,1,512)
            assert rc==-1 and c.get_errno()==errno.EFAULT,(rc,c.get_errno())
            assert space(fd)==capacity;return
        before=action in ('signal-before','format-before')
        fill=capacity if before else capacity//2
        assert os.write(fd,b'\x12'*fill)==fill
        writer=Writer(fd,512 if before else capacity);writers.append(writer)
        if before:
            time.sleep(.05);assert writer.thread.is_alive() and space(fd)==0
        else:wait_full(fd,writer)
        if action.startswith('signal'):
            interrupt(writer);result,error=writer.join()
            if before:assert (result,error)==(-1,errno.EINTR),(result,error)
            else:assert result==capacity//2,(result,error)
        elif action in ('reset','pause','two-writers'):
            if action=='two-writers':
                other=Writer(fd,512);writers.append(other)
                time.sleep(.05);assert other.thread.is_alive()
            if action=='pause':integer(fd,0x40045010,0)
            else:fcntl.ioctl(fd,0x5000)  # RESET
            assert writer.join()[0]==capacity//2,writer.result
            if action=='two-writers':
                assert other.join()[0]==512,other.result
                assert space(fd)==capacity-512
            else:assert space(fd)==(0 if action=='pause' else capacity)
        elif action=='format-before':
            fcntl.ioctl(fd,0x5000)
            try:
                assert integer(fd,0xC0045002,48000)==48000
                print('DSP_FORMAT_OUTCOME changed-before-acceptance',flush=True)
            except OSError as error:
                # RESET frees space. The writer may legitimately refill it
                # before the following format ioctl acquires the mutex.
                assert error.errno==errno.EBUSY,error
                assert integer(fd,0xC0045002,0)==44100
                print('DSP_FORMAT_OUTCOME writer-won-reset-race',flush=True)
            assert writer.join()[0]==512,writer.result
        elif action=='fault-after':
            assert libc.mprotect(writer.address,writer.size,0)==0
            protected=writer
            integer(fd,0x40045010,2)  # Enable without changing the control epoch.
            assert writer.join()[0]==capacity//2,writer.result
        else:raise AssertionError(action)
    finally:
        if protected:
            assert libc.mprotect(protected.address,protected.size,3)==0
        for writer in writers:
            if writer.thread.is_alive():
                interrupt(writer);fcntl.ioctl(fd,0x5000);writer.thread.join(2)
                if writer.thread.is_alive():os._exit(124)
            writer.close()
        os.close(fd)


def main():
    finished=threading.Event()
    def watchdog():
        if not finished.wait(60):os._exit(124)
    threading.Thread(target=watchdog,daemon=True).start()
    results=[]
    try:
        for name in ['fault-before','signal-before','signal-after','reset','pause','format-before','fault-after','two-writers']:
            control_case(name);results.append(name);print('DSP_CONTROL_PASS',name,flush=True)
        print(json.dumps(dict(passed=True,cases=results)),flush=True)
    finally:finished.set()


if __name__=='__main__':main()
