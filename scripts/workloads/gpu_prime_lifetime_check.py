#!/usr/bin/env python3
"""Guest-only PRIME shared backing, exporter close and mapping lifetime probe.

This checks reference ownership. Pair it with host backing-counter checks to
verify reclamation after the command and every mapping/capability have drained.
"""
import argparse
import ctypes
import errno
import fcntl
import json
import mmap
import os
import struct


def ioctl(fd, number, payload):
    data=bytearray(payload)
    fcntl.ioctl(fd,(3<<30)|(len(data)<<16)|(ord('d')<<8)|number,data,True)
    return data


def create(fd):
    data=ioctl(fd,0x44,struct.pack('<14I',0,64,1<<4,128,1,1,1,0,0,0,0,0,128,0))
    values=struct.unpack('<14I',data)
    return values[10],values[11]


def mapped(fd, handle):
    offset=struct.unpack('<QII',ioctl(fd,0x41,struct.pack('<QII',0,handle,0)))[0]
    return mmap.mmap(fd,4096,flags=mmap.MAP_SHARED,
                     prot=mmap.PROT_READ|mmap.PROT_WRITE,offset=offset,trackfd=False)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--brook-guest',required=True,action='store_true')
    parser.parse_args()
    source=target=prime=-1
    a=b=None
    cases={}
    try:
        source=os.open('/dev/dri/renderD128',os.O_RDWR)
        target=os.open('/dev/dri/renderD128',os.O_RDWR)
        handle,resource=create(source)
        a=mapped(source,handle)
        pattern=bytes(range(128));a[:128]=pattern
        prime=struct.unpack('<IIi',ioctl(source,0x2d,struct.pack('<IIi',handle,0,-1)))[2]
        imported=struct.unpack('<IIi',ioctl(target,0x2e,struct.pack('<IIi',0,0,prime)))[0]
        b=mapped(target,imported)
        cases['importer_maps_exporter_contents']=b[:128]==pattern
        b[7]=0xcc
        cases['importer_write_shared_with_exporter']=a[7]==0xcc
        if not all(cases.values()):
            print('GPU_PRIME_LIFETIME_RESULT',json.dumps({'pass':False,'cases':cases}),flush=True)
            return 1
        a.close();a=None
        ioctl(source,0x09,struct.pack('<II',handle,0))
        os.close(source);source=-1
        os.close(prime);prime=-1

        # Source GEM, all source mappings, source render FD and exported FD
        # are now gone. The imported GEM must retain both backing and context.
        expected=bytearray(pattern);expected[7]=0xcc
        cases['contents_after_exporter_and_prime_close']=b[:128]==expected
        for number in (0x47,0x46):
            ioctl(target,number,struct.pack('<11I',imported,0,0,0,128,1,1,0,0,0,0))
            if number==0x47:b[:128]=bytes(128)
        cases['host_roundtrip_after_exporter_close']=b[:128]==expected

        # Numeric presentation must resolve an exported resource capability.
        # Use a nonexistent window ID: no real window or pixels are changed.
        private_handle,private_resource=create(target)
        libc=ctypes.CDLL(None,use_errno=True);libc.syscall.restype=ctypes.c_long
        def present(gres,width=128):
            ctypes.set_errno(0)
            result=libc.syscall(ctypes.c_long(525),ctypes.c_long(0x7ffffffe),
                                ctypes.c_long(gres),ctypes.c_long(width),ctypes.c_long(1))
            return result,ctypes.get_errno()
        cases['unexported_present_rejected']=present(private_resource)==(-1,errno.EINVAL)
        cases['unowned_present_rejected']=present(private_resource+0x40000000)==(-1,errno.EINVAL)
        cases['oversized_present_rejected']=present(resource,129)==(-1,errno.EINVAL)
        cases['owned_export_resolves_before_window_lookup']=present(resource)==(-1,errno.ENOENT)
        ioctl(target,0x09,struct.pack('<II',private_handle,0))
        ioctl(target,0x09,struct.pack('<II',imported,0))
        os.close(target);target=-1
        cases['mapping_after_all_gems_and_fds_close']=b[:128]==expected
        child=os.fork()
        if child==0:
            try:
                if b[:128]!=expected:os._exit(2)
                b[19]=0xda
                b.close();os._exit(0)
            except BaseException:os._exit(3)
        _,status=os.waitpid(child,0)
        cases['shared_fork_after_exporter_importer_close']=status==0 and b[19]==0xda
        b.close();b=None
        print('GPU_PRIME_LIFETIME_RESULT',json.dumps({'pass':all(cases.values()),
              'cases':cases,'exported_resource_retirement':'requires host backing-counter endpoint check'}),flush=True)
        return int(not all(cases.values()))
    finally:
        if a is not None:a.close()
        if b is not None:b.close()
        for fd in (prime,target,source):
            if fd>=0:os.close(fd)


if __name__=='__main__':
    raise SystemExit(main())
