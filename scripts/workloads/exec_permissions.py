#!/usr/bin/env python3
"""Owned execve permission matrix; ELF fixtures use only the exit syscall.

Default Brook mode requires root for setup and credential actors. --host tests
only the current Linux owner; neither mode changes existing files/accounts.
No set-ID, capabilities, noexec mount or full executable-identity claim.
"""
import argparse
import errno
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import threading
import time

A,B,G=66536,66537,131172
MODES=[0o000,0o100,0o010,0o001,0o500,0o050,0o005,0o111,0o444,0o700,0o070,0o007,0o011,0o101,0o640,0o777]


def elf(status,interpreter=None,dynamic=False):
    data=bytearray(0x220)
    base=0 if dynamic else 0x400000
    ident=b'\x7fELF\x02\x01\x01'+bytes(9)
    count=2 if interpreter else 1
    data[:64]=struct.pack('<16sHHIQQQIHHHHHH',ident,3 if dynamic else 2,62,1,
                          base+0x200,64,0,0,64,56,count,64,0,0)
    data[64:120]=struct.pack('<IIQQQQQQ',1,5,0,base,base,len(data),len(data),4096)
    if interpreter:
        name=os.fsencode(interpreter)+b'\0'
        assert len(name)<128
        data[120:176]=struct.pack('<IIQQQQQQ',3,4,0x180,base+0x180,base+0x180,len(name),len(name),1)
        data[0x180:0x180+len(name)]=name
    # mov edi,status; mov eax,SYS_exit; syscall
    data[0x200:0x20c]=b'\xbf'+struct.pack('<I',status)+b'\xb8\x3c\0\0\0\x0f\x05'
    return bytes(data)


def allowed(mode,euid,egid,groups,owner,group):
    if euid==0:return bool(mode&0o111)
    bits=(mode>>6)&7 if euid==owner else (mode>>3)&7 if egid==group or group in groups else mode&7
    return bool(bits&1)


def execute(path,threads=False):
    read,write=os.pipe();pid=os.fork()
    if pid==0:
        os.close(read)
        try:
            stop=threading.Event();ready=threading.Event();worker=None
            signal.signal(signal.SIGUSR1,lambda *_:None)
            if threads:
                def work():
                    ready.set()
                    while not stop.wait(.005):pass
                worker=threading.Thread(target=work);worker.start();assert ready.wait(5)
            try:os.execve(str(path),[str(path)],{'PATH':'/nix/profile/bin:/bin','HOME':'/data/home'})
            except OSError as error:
                survived=not threads or worker.is_alive()
                handler=callable(signal.getsignal(signal.SIGUSR1))
                stop.set()
                if worker:worker.join(5)
                os.write(write,json.dumps({'errno':error.errno,'thread_survived':survived,'handler_preserved':handler}).encode())
                os._exit(0)
        except BaseException:
            __import__('traceback').print_exc();os._exit(98)
    os.close(write);data=b''
    while chunk:=os.read(read,4096):data+=chunk
    os.close(read);waited,status=os.waitpid(pid,0)
    assert waited==pid
    if data:
        result=json.loads(data);result['wait_status']=status;return result
    if os.WIFEXITED(status):return {'exit':os.WEXITSTATUS(status)}
    return {'signal':os.WTERMSIG(status)}


def probe(root,owner,group,euid,egid,groups):
    rows=[]
    def check(label,path,want,threads=False):
        got=execute(path,threads)
        if want>=1000:
            match=(got.get('errno')==want-1000 and got.get('wait_status')==0 and
                   got.get('thread_survived') and got.get('handler_preserved'))
        else:match=got=={'exit':want}
        rows.append({'case':label,'actual':got,'expected':want,'matches':bool(match)})
    for mode in MODES:
        ok=allowed(mode,euid,egid,groups,owner,group)
        for kind,code in [('elf',41),('script',43),('dynamic',43)]:
            check(f'{kind}-{mode:03o}',root/f'{kind}-{mode:03o}',code if ok else 1000+errno.EACCES)
    for label,code in [('missing-interpreter',errno.ENOENT),('bad-interpreter',errno.ELIBBAD),('directory-interpreter',errno.EACCES),('short-interpreter',errno.EIO)]:
        check(label,root/label,1000+code,threads=True)
    check('directory-main',root/'directory',1000+errno.EACCES)
    check('symlink-allowed',root/'link-allowed',41)
    check('symlink-denied',root/'link-denied',1000+errno.EACCES)
    check('ancestor-search',root/'private/program',41 if euid in (0,owner) else 1000+errno.EACCES)
    return rows


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--host',action='store_true');ap.add_argument('--root',default='/data')
    ap.add_argument('--case',required=True);ap.add_argument('--record-baseline',action='store_true');ap.add_argument('--output')
    args=ap.parse_args();assert len(args.case)==16 and all(c in '0123456789abcdef' for c in args.case)
    assert args.host or os.geteuid()==0
    root=Path(args.root)/('brook-exec-'+args.case);root.mkdir(mode=0o755);root.chmod(0o755)
    owner,group=(os.getuid(),os.getgid()) if args.host else (A,G)
    results={};started=time.monotonic()
    def file(path,data,mode):
        path.write_bytes(data)
        if not args.host:os.chown(path,owner,group)
        path.chmod(mode)
    try:
        file(root/'interpreter',elf(43,dynamic=True),0o777)
        for mode in MODES:
            file(root/f'elf-{mode:03o}',elf(41),mode)
            file(root/f'script-{mode:03o}',b'#!'+os.fsencode(root/'interpreter')+b'\n',mode)
            file(root/f'loader-{mode:03o}',elf(43,dynamic=True),mode)
            file(root/f'dynamic-{mode:03o}',elf(41,root/f'loader-{mode:03o}'),0o777)
        file(root/'not-elf',b'not an ELF interpreter'+bytes(128),0o777)
        file(root/'short-elf',b'short ELF header',0o777)
        (root/'directory').mkdir();(root/'directory').chmod(0o755)
        file(root/'missing-interpreter',elf(41,root/'missing'),0o777)
        file(root/'bad-interpreter',elf(41,root/'not-elf'),0o777)
        file(root/'short-interpreter',elf(41,root/'short-elf'),0o777)
        file(root/'directory-interpreter',elf(41,root/'directory'),0o777)
        file(root/'allowed',elf(41),0o111);file(root/'denied',elf(41),0o444)
        (root/'link-allowed').symlink_to('allowed');(root/'link-denied').symlink_to('denied')
        (root/'private').mkdir();file(root/'private/program',elf(41),0o777)
        if not args.host:os.chown(root/'private',owner,group)
        (root/'private').chmod(0o700)
        if args.host:
            results['host-owner']=probe(root,owner,group,os.geteuid(),os.getegid(),os.getgroups())
        else:
            actors=[('root',0,0,0,0,[]),('owner',A,A,100,100,[]),('primary',B,B,G,G,[]),
                    ('supplementary',B,B,100,100,[G]),('owner-with-group',A,A,G,G,[]),('other',B,B,100,100,[]),
                    ('real-owner',A,B,100,100,[]),('effective-owner',B,A,100,100,[]),
                    ('real-root',0,B,0,100,[]),('effective-root',B,0,100,0,[]),
                    ('real-primary',B,B,G,100,[]),('effective-primary',B,B,100,G,[])]
            for label,ru,eu,rg,eg,groups in actors:
                read,write=os.pipe();pid=os.fork()
                if pid==0:
                    os.close(read)
                    try:
                        os.setgroups(groups);os.setresgid(rg,eg,0);os.setresuid(ru,eu,0)
                        rows=probe(root,owner,group,eu,eg,groups);data=json.dumps(rows).encode()
                        while data:data=data[os.write(write,data):]
                        os._exit(0)
                    except BaseException:__import__('traceback').print_exc();os._exit(99)
                os.close(write);data=b''
                while chunk:=os.read(read,65536):data+=chunk
                os.close(read);assert os.waitpid(pid,0)==(pid,0),(label,'actor failure')
                results[label]=json.loads(data)
        total=mismatches=0
        for label,rows in results.items():
            bad=[r for r in rows if not r['matches']];mismatches+=len(bad);total+=len(rows)
            print('EXEC_ACTOR',label,'cases',len(rows),'mismatches',len(bad),'examples',json.dumps(bad[:3]),flush=True)
        if args.output:Path(args.output).write_text(json.dumps(results,indent=2)+'\n')
        print('EXEC_TOTAL',total,'MISMATCHES',mismatches,'SECONDS',round(time.monotonic()-started,3),flush=True)
        assert mismatches>0 if args.record_baseline else mismatches==0
        print('EXEC_BASELINE_RECORDED' if args.record_baseline else 'EXEC_PERMISSIONS_PASS',flush=True)
    finally:
        if (root/'private').exists():(root/'private').chmod(0o700)
        shutil.rmtree(root);os.sync()


if __name__=='__main__':main()
