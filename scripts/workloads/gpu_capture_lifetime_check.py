#!/usr/bin/env python3
"""Guest half of a host-observed capture/reuse/SHM/minimized regression.

Run only on an isolated Brook VM. The host must inspect every named phase and
create the matching file in --control before this bounded command continues.
This guest half alone does not establish reclamation; the host must also check
backing counters after the source references have drained.
"""
import argparse
import ctypes
import fcntl
import json
import mmap
import os
from pathlib import Path
import struct
import time


def ioctl(fd, number, payload):
    value=bytearray(payload)
    fcntl.ioctl(fd,(3<<30)|(len(value)<<16)|(ord('d')<<8)|number,value,True)
    return value


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--brook-guest',action='store_true',required=True)
    p.add_argument('--control',required=True)
    p.add_argument('--format',choices=('xrgb','argb'),default='xrgb')
    args=p.parse_args();control=Path(args.control);control.mkdir(exist_ok=False)
    libc=ctypes.CDLL(None,use_errno=True);libc.syscall.restype=ctypes.c_long
    def call(number,*values):
        rc=libc.syscall(ctypes.c_long(number),*[ctypes.c_long(v) for v in values])
        if rc<0:raise OSError(ctypes.get_errno(),f'Brook syscall {number}')
        return rc
    title=ctypes.create_string_buffer(b'Capture lifetime probe')
    output=ctypes.create_string_buffer(16)
    call(506,320,240,ctypes.addressof(title),ctypes.addressof(output),0)
    window,stride,address=struct.unpack('<IIQ',output.raw);assert window and address and stride>=320
    def checkpoint(name):
        print('CAPTURE_PROBE_PHASE',name,json.dumps({'window':window,'stride':stride,'control':str(control)}),flush=True)
        end=time.monotonic()+180
        while not (control/name).exists():
            if time.monotonic()>end:raise TimeoutError(name)
            time.sleep(.1)
    def capture(color,overwrite=None):
        fd=os.open('/dev/dri/renderD128',os.O_RDWR);prime=-1;memory=None;handle=0
        try:
            source_format=1 if args.format=='argb' else 2
            fields=struct.unpack('<14I',ioctl(fd,0x44,struct.pack('<14I',2,source_format,10,64,64,1,1,0,0,0,0,0,16384,0)))
            handle,resource=fields[10:12];assert handle and resource
            offset=struct.unpack('<QII',ioctl(fd,0x41,struct.pack('<QII',0,handle,0)))[0]
            memory=mmap.mmap(fd,16384,flags=mmap.MAP_SHARED,prot=mmap.PROT_READ|mmap.PROT_WRITE,offset=offset,trackfd=False)
            transfer=struct.pack('<11I',handle,0,0,0,64,64,1,0,0,0,0)
            memory[:]=struct.pack('<I',color)*4096;ioctl(fd,0x47,transfer)
            prime=struct.unpack('<IIi',ioctl(fd,0x2d,struct.pack('<IIi',handle,0,-1)))[2];assert prime>=0
            call(525,window,resource,64,64)
            if overwrite is not None:
                memory[:]=struct.pack('<I',overwrite)*4096;ioctl(fd,0x47,transfer)
                memory[:]=bytes(16384);ioctl(fd,0x46,transfer)
                assert all((value[0]&0xffffff)==(overwrite&0xffffff)
                           for value in struct.iter_unpack('<I',memory[:]))
        finally:
            if memory is not None:memory.close()
            if handle:ioctl(fd,0x09,struct.pack('<II',handle,0))
            if prime>=0:os.close(prime)
            os.close(fd)
    try:
        capture(0xff00ff00,0xff0000ff);checkpoint('retained-green')
        pixels=(ctypes.c_uint32*(stride*240)).from_address(address)
        for i in range(len(pixels)):pixels[i]=0xffff0000
        call(508,window);checkpoint('shm-red')
        capture(0xff0000ff,0xff00ff00);checkpoint('retained-blue')
        checkpoint('minimize-now')
        capture(0xffffff00,0xffff00ff);checkpoint('minimized-yellow')
        checkpoint('restored-yellow')
        print('CAPTURE_LIFETIME_GUEST_PASS source-overwrite-and-close GL/SHM/GL minimized-capture',flush=True)
    finally:
        call(507,window)


if __name__=='__main__':
    main()
