#!/usr/bin/env python3
"""Guest negative capture checks; actual rendered imports are tested by WSI apps.

The untyped allocation deliberately has no Vulkan image and is not shareable.
All capture requests must be rejected before host image import. Current-frame
WRITE admission and queued-read conflicts are covered by scene-service tests;
the actual WSI workloads exercise successful exported image presentation.
"""
import argparse
import ctypes
import errno
import fcntl
import json
import os
import struct


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--brook-guest',action='store_true',required=True)
    parser.parse_args()
    libc=ctypes.CDLL(None,use_errno=True);libc.syscall.restype=ctypes.c_long
    def call(number,*values):
        result=libc.syscall(ctypes.c_long(number),*[ctypes.c_long(v) for v in values])
        if result<0:raise OSError(ctypes.get_errno(),'Brook syscall')
        return result
    def ioctl(fd,nr,payload):
        data=bytearray(payload)
        fcntl.ioctl(fd,0xc0006400|(len(data)<<16)|nr,data,True)
        return data
    checks=0
    def reject(code,fn):
        nonlocal checks
        try:fn()
        except OSError as error:assert error.errno==code,(code,error)
        else:raise AssertionError('unsafe capture accepted')
        checks+=1
    def ctl(op,payload):
        data=ctypes.create_string_buffer(payload)
        return call(0xB07,scene,op,ctypes.addressof(data),len(payload),0,0)
    title=ctypes.create_string_buffer(b'DMA-BUF capture validation');out=ctypes.create_string_buffer(16)
    call(506,320,240,ctypes.addressof(title),ctypes.addressof(out),0)
    window=struct.unpack('<IIQ',out.raw)[0]
    drm=scene=prime=-1;gem=0
    try:
        scene=call(0xB06,window,1,0,0,0,0)
        drm=os.open('/dev/dri/renderD128',os.O_RDWR|os.O_CLOEXEC)
        params=ctypes.create_string_buffer(struct.pack('<6Q',1,4,2,64,3,0))
        ioctl(drm,0x4b,struct.pack('<IIQ',3,0,ctypes.addressof(params)))
        fields=struct.unpack('<4IQ2I2Q',ioctl(drm,0x4a,struct.pack('<4IQ2I2Q',2,1,0,0,8192,0,0,0,0)))
        gem,resource=fields[2:4];assert gem and resource
        prime=struct.unpack('<IIi',ioctl(drm,0x2d,struct.pack('<IIi',gem,3,-1)))[2]
        def capture(**changes):
            values=dict(drm=drm,gem=gem,width=32,height=32,fourcc=0x34325258,stride=128,offset=0,flags=0,modifier=0)
            values.update(changes)
            return ctl(6,struct.pack('<i7IQ',*values.values()))
        reject(errno.EINVAL,capture) # no exportable Vulkan image behind this blob
        reject(errno.EBADF,lambda:capture(drm=-1))
        for changes in ({'gem':0},{'width':0},{'height':16385},{'fourcc':0},{'stride':124},
                        {'stride':130},{'offset':2},{'offset':8192},{'height':65},
                        {'flags':1},{'modifier':0x00ffffffffffffff},{'modifier':1}):
            reject(errno.EINVAL,lambda changes=changes:capture(**changes))
        reject(errno.EFAULT,lambda:call(0xB07,scene,6,1,40,0,0))
        reject(errno.EINVAL,lambda:ctl(2,struct.pack('<i5I',drm,gem,32,32,2,0)))
        reject(errno.EINVAL,lambda:call(525,window,resource,32,32,0,0))
    finally:
        for fd in (prime,):
            if fd>=0:os.close(fd)
        if gem:ioctl(drm,0x09,struct.pack('<II',gem,0))
        if drm>=0:os.close(drm)
        if scene>=0:os.close(scene)
        call(507,window,0,0,0,0,0)
    print(json.dumps(dict(passed=True,rejected_requests=checks,exportable_image_required=True,host_image_import_attempted=False)))


if __name__=='__main__':main()
