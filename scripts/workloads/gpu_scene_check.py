#!/usr/bin/env python3
"""Guest retained-scene probe; requires host observation of each named phase."""
import argparse
import array
import ctypes
import fcntl
import json
import mmap
import os
from pathlib import Path
import socket
import struct
import time


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--brook-guest',action='store_true',required=True)
    parser.add_argument('--control',required=True)
    args=parser.parse_args();control=Path(args.control);control.mkdir(exist_ok=False)
    libc=ctypes.CDLL(None,use_errno=True);libc.syscall.restype=ctypes.c_long
    def call(number,*values):
        rc=libc.syscall(ctypes.c_long(number),*[ctypes.c_long(value) for value in values])
        if rc<0:raise OSError(ctypes.get_errno(),f'Brook syscall {number}')
        return rc
    def ctl(fd,op,payload):
        buf=ctypes.create_string_buffer(payload)
        return call(0xB07,fd,op,ctypes.addressof(buf),len(payload),0,0)
    def stats(fd):
        buf=ctypes.create_string_buffer(88);call(0xB07,fd,0,ctypes.addressof(buf),88,0,0)
        fields=struct.unpack('<4I5Q8I',buf.raw)
        names=('abi','max_layers','max_dimension','version_slots','pixel_bytes','pixel_limit','pins',
               'snapshot_bytes','snapshot_limit','published','reserved','idle','quarantined','in_flight',
               'max_in_flight','scene_count','max_scenes')
        return dict(zip(names,fields))
    def expect_error(code,fn):
        try:fn()
        except OSError as error:assert error.errno==code,(error,code)
        else:raise AssertionError(f'expected errno {code}')
    def upload(fd,color,format=1):
        pixels=ctypes.create_string_buffer(struct.pack('<I',color)*4096)
        version=ctl(fd,1,struct.pack('<QQ4I',ctypes.addressof(pixels),16384,64,64,256,format))
        assert version>0;ctypes.memset(ctypes.addressof(pixels),0x55,16384)
        return version
    def present(fd,*versions):
        entries=b''.join(struct.pack('<Q4I2i3I2i3I',v,0,0,64,64,0,0,64,64,255,0,0,64,64,0) for v in versions)
        layers=ctypes.create_string_buffer(entries)
        return ctl(fd,3,struct.pack('<Q4I',ctypes.addressof(layers),len(versions),64,64,0))
    def release(fd,version):return call(0xB07,fd,4,version,0,0,0)
    def checkpoint(name):
        print('SCENE_PROBE_PHASE',name,json.dumps({'window':window,'control':str(control)}),flush=True)
        end=time.monotonic()+180
        while not (control/name).exists():
            if time.monotonic()>end:raise TimeoutError(name)
            time.sleep(.1)
    def ioctl(fd,number,payload):
        value=bytearray(payload);fcntl.ioctl(fd,(3<<30)|(len(value)<<16)|(ord('d')<<8)|number,value,True);return value
    title=ctypes.create_string_buffer(b'Retained scene probe');output=ctypes.create_string_buffer(16)
    call(506,320,240,ctypes.addressof(title),ctypes.addressof(output),0)
    window,stride,address=struct.unpack('<IIQ',output.raw);assert window and stride and address
    descriptors=[]
    def scene():
        fd=call(0xB06,window,1,0,0,0,0);descriptors.append(fd)
        assert fcntl.fcntl(fd,fcntl.F_GETFD)&fcntl.FD_CLOEXEC
        return fd
    def close(fd):os.close(fd);descriptors.remove(fd)
    try:
        fd=scene();a=upload(fd,0xffff0000);b=upload(fd,0xff0000ff)
        expect_error(14,lambda:ctl(fd,1,struct.pack('<QQ4I',1,16384,64,64,256,1)))
        expect_error(9,lambda:ctl(fd,2,struct.pack('<i5I',-1,1,64,64,1,0)))
        assert present(fd,a)==0;checkpoint('red')
        # Capture through an explicit DRM/GEM capability. Export is unnecessary;
        # the resource lease is independent of descriptor and GEM destruction.
        drm=os.open('/dev/dri/renderD128',os.O_RDWR);memory=None;handle=0
        try:
            fields=struct.unpack('<14I',ioctl(drm,0x44,struct.pack('<14I',2,1,10,64,64,1,1,0,0,0,0,0,16384,0)))
            handle=fields[10];assert handle and fields[11]
            offset=struct.unpack('<QII',ioctl(drm,0x41,struct.pack('<QII',0,handle,0)))[0]
            memory=mmap.mmap(drm,16384,flags=mmap.MAP_SHARED,prot=mmap.PROT_READ|mmap.PROT_WRITE,offset=offset,trackfd=False)
            transfer=struct.pack('<11I',handle,0,0,0,64,64,1,0,0,0,0)
            memory[:]=struct.pack('<I',0xff00ff00)*4096;ioctl(drm,0x47,transfer)
            green=ctl(fd,2,struct.pack('<i5I',drm,handle,64,64,1,0));assert green>0
            memory[:]=struct.pack('<I',0xffff00ff)*4096;ioctl(drm,0x47,transfer)
            memory[:]=bytes(16384);ioctl(drm,0x46,transfer)
            assert all(p[0]==0xffff00ff for p in struct.iter_unpack('<I',memory[:]))
        finally:
            if memory is not None:memory.close()
            if handle:ioctl(drm,0x09,struct.pack('<II',handle,0))
            os.close(drm)
        assert present(fd,green)==0;checkpoint('retained-green')
        alpha=upload(fd,0x80008000);assert present(fd,a,alpha)==0;checkpoint('alpha-over-red')
        assert present(fd,b)==0;release(fd,b);expect_error(22,lambda:present(fd,b));checkpoint('released-blue')
        alias=os.dup(fd);descriptors.append(alias)
        pid=os.fork()
        if pid==0:
            try:
                assert stats(alias)['published']>=3
                os.close(fd);os.close(alias);os._exit(0)
            except BaseException:os._exit(1)
        assert os.waitpid(pid,0)[1]==0
        # Both descriptor aliases are close-on-exec under Python's os.dup.
        assert fcntl.fcntl(alias,fcntl.F_GETFD)&fcntl.FD_CLOEXEC
        exec_check="""import ctypes,errno
c=ctypes.CDLL(None,use_errno=True);c.syscall.restype=ctypes.c_long
b=ctypes.create_string_buffer(88)
for fd in FD_VALUES:
 r=c.syscall(*[ctypes.c_long(v) for v in (0xB07,fd,0,ctypes.addressof(b),88,0,0)])
 assert r==-1 and ctypes.get_errno()==errno.EBADF
""".replace('FD_VALUES',repr((fd,alias)))
        pid=os.fork()
        if pid==0:os.execl('/nix/profile/bin/python3','python3','-c',exec_check)
        assert os.waitpid(pid,0)[1]==0
        left,right=socket.socketpair()
        try:
            for endpoint in (left,right):
                assert endpoint.getsockname()==''
                assert endpoint.getsockopt(socket.SOL_SOCKET,socket.SO_TYPE)==socket.SOCK_STREAM
                assert endpoint.getsockopt(socket.SOL_SOCKET,socket.SO_DOMAIN)==socket.AF_UNIX
                assert endpoint.getsockopt(socket.SOL_SOCKET,socket.SO_PROTOCOL)==0
            short=ctypes.create_string_buffer(1);length=ctypes.c_uint(1)
            call(51,left.fileno(),ctypes.addressof(short),ctypes.addressof(length))
            assert short.raw==b'\1' and length.value==2
            expect_error(14,lambda:call(51,left.fileno(),ctypes.addressof(short),0))
            length.value=0xffffffff
            expect_error(22,lambda:call(51,left.fileno(),ctypes.addressof(short),ctypes.addressof(length)))
            left.sendmsg([b'S'],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',[fd]))])
            close(fd);close(alias)
            data,ancillary,flags,_=right.recvmsg(1,socket.CMSG_SPACE(4),socket.MSG_CMSG_CLOEXEC)
            assert data==b'S' and not flags&socket.MSG_CTRUNC and len(ancillary)==1
            received=array.array('i');received.frombytes(ancillary[0][2]);assert len(received)==1
            fd=received[0];descriptors.append(fd)
            assert fcntl.fcntl(fd,fcntl.F_GETFD)&fcntl.FD_CLOEXEC
            assert stats(fd)['published']>=3
        finally:left.close();right.close()
        listener=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
        client=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);accepted=None
        try:
            local_name=str(control/'named.sock');listener.bind(local_name);listener.listen(1)
            assert listener.getsockname()==local_name
            client.connect(local_name);accepted,_=listener.accept()
            assert client.getsockname()=='' and accepted.getsockname()==local_name
        finally:
            if accepted is not None:accepted.close()
            client.close();listener.close()
        replacement=scene();expect_error(116,lambda:present(fd,a))
        # Move the captured image to a replacement scene after the producer GEM
        # and DRM FD are gone. These errors must leave the source public claim.
        expect_error(9,lambda:ctl(replacement,5,struct.pack('<iIQ',-1,0,green)))
        expect_error(22,lambda:ctl(replacement,5,struct.pack('<iIQ',fd,1,green)))
        expect_error(22,lambda:ctl(replacement,5,struct.pack('<iIQ',fd,0,0)))
        migrated=ctl(replacement,5,struct.pack('<iIQ',fd,0,green));assert migrated>0
        yellow=upload(replacement,0xffffff00)
        # The clone must have independent storage even when its source scene
        # closes and its source image storage is retired.
        close(fd)
        assert present(replacement,migrated)==0;checkpoint('cloned-green')
        translucent=upload(replacement,0x80008000)
        copied_alpha=ctl(replacement,5,struct.pack('<iIQ',replacement,0,translucent))
        release(replacement,translucent)
        red=upload(replacement,0xffff0000)
        assert present(replacement,red,copied_alpha)==0;checkpoint('cloned-alpha-over-red')
        release(replacement,migrated);release(replacement,copied_alpha);release(replacement,red)
        opaque=upload(replacement,0x000000ff,format=2)
        copied_opaque=ctl(replacement,5,struct.pack('<iIQ',replacement,0,opaque))
        release(replacement,opaque)
        assert present(replacement,copied_opaque)==0;checkpoint('cloned-opaque-blue')
        release(replacement,copied_opaque)
        assert present(replacement,yellow)==0
        end=time.monotonic()+30
        while stats(replacement)['scene_count']!=1:
            if time.monotonic()>end:raise TimeoutError('old scene did not drain')
            time.sleep(.1)
        checkpoint('replacement-yellow')
        # Repeated updates reuse completed storage with fresh, nonwrapping IDs.
        previous=yellow;release(replacement,yellow)
        retained_bytes=stats(replacement)['pixel_bytes']
        assert retained_bytes<=8*16384
        for _ in range(128):
            version=upload(replacement,0xffffff00);assert version>previous;previous=version
            assert present(replacement,version)==0;release(replacement,version)
        final=stats(replacement)
        assert final['pixel_bytes']<=retained_bytes and not final['pins'] and not final['quarantined']
        assert not final['snapshot_bytes'] and final['scene_count']==1
        assert not final['published'] and not final['reserved']
        print('SCENE_API_GUEST_PASS',json.dumps(final,sort_keys=True),flush=True)
        close(replacement);checkpoint('closed-yellow')
    finally:
        for fd in descriptors:os.close(fd)
        call(507,window)


if __name__=='__main__':main()
