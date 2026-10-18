#!/usr/bin/env python3
"""Private access/faccessat/faccessat2 permission matrix; no account isolation claim."""
import argparse
import ctypes
import errno
import json
import os
from pathlib import Path
import re

A,B,G=66536,66537,131172
libc=ctypes.CDLL(None,use_errno=True);libc.syscall.restype=ctypes.c_long
EACCESS,NOFOLLOW,EMPTY=0x200,0x100,0x1000
MODES=[0o000,0o700,0o070,0o007,0o640,0o604,0o460,0o021,0o102,0o011,0o444,0o222,0o111,0o777]


def access(path,requested,flags=None,fd=-100,legacy=False):
    p=ctypes.c_char_p(os.fsencode(path))
    ctypes.set_errno(0)
    if flags is None:result=libc.syscall(ctypes.c_long(21),p,ctypes.c_int(requested))
    elif legacy:result=libc.syscall(ctypes.c_long(269),ctypes.c_int(fd),p,ctypes.c_int(requested))
    else:result=libc.syscall(ctypes.c_long(439),ctypes.c_int(fd),p,ctypes.c_int(requested),ctypes.c_int(flags))
    return ctypes.get_errno() if result<0 else 0


def expected(mode,requested,uid,gid,groups,owner,group,directory=False):
    if uid==0:
        bits=7
        if requested&1 and not directory and not mode&0o111:return errno.EACCES
    else:bits=(mode>>6)&7 if uid==owner else (mode>>3)&7 if gid==group or group in groups else mode&7
    return 0 if bits&requested==requested else errno.EACCES


def probe(root,ids,groups,owner,group,held,unlinked,host):
    ru,eu,rg,eg=ids;rows=[]
    def check(name,actual,want):rows.append(dict(case=name,actual=actual,expected=want,matches=actual==want))
    for mode in MODES:
        p=root/('mode-%03o'%mode)
        for requested in range(8):
            for label,flags,legacy in [('access',None,False),('faccessat',0,True),('faccessat2',0,False),('effective',EACCESS,False)]:
                uid,gid=(eu,eg) if flags==EACCESS else (ru,rg)
                check('%s-%03o-%d'%(label,mode,requested),access(p,requested,flags,legacy=legacy),expected(mode,requested,uid,gid,groups,owner,group))
    if not host:
        for label,flags in [('real',0),('effective',EACCESS)]:
            uid,gid=(ru,rg) if not flags else (eu,eg)
            can_search=expected(0o700,1,uid,gid,groups,A,G,True)==0
            for requested in range(8):
                check(label+'-ancestor-'+str(requested),access(root/'private/file',requested,flags),0 if can_search else errno.EACCES)
                check(label+'-empty-fd-'+str(requested),access('',requested,flags|EMPTY,held),expected(0o460,requested,uid,gid,groups,A,G))
                check(label+'-unlinked-fd-'+str(requested),access('',requested,flags|EMPTY,unlinked),expected(0o640,requested,uid,gid,groups,A,G))
                # Link mode is 0777; do not substitute its mode-000 referent.
                check(label+'-nofollow-'+str(requested),access(root/'link',requested,flags|NOFOLLOW),0)
        check('invalid-mode',access(root/'mode-777',8,0),errno.EINVAL)
        check('invalid-flags',access(root/'mode-777',0,0x40000000),errno.EINVAL)
        check('bad-empty-fd',access('',0,EMPTY,-1),errno.EBADF)
        check('empty-without-flag',access('',0,0,held),errno.ENOENT)
        check('absolute-ignores-dirfd',access(root/'mode-777',0,0,-1),0)
    return rows


def cleanup(root):
    root.chmod(0o700)
    for p in root.iterdir():
        if p.is_dir() and not p.is_symlink():cleanup(p)
        else:p.unlink()
    root.rmdir()


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--host',action='store_true');ap.add_argument('--root',default='/data');ap.add_argument('--case',required=True)
    ap.add_argument('--record-baseline',action='store_true');ap.add_argument('--output')
    args=ap.parse_args();assert re.fullmatch('[0-9a-f]{16}',args.case);assert args.host or os.geteuid()==0
    root=Path(args.root)/('brook-access-'+args.case);root.mkdir(mode=0o755);root.chmod(0o755)
    held=unlinked=-1;results={}
    try:
        owner,group=(os.getuid(),os.getgid()) if args.host else (A,G)
        for mode in MODES:
            p=root/('mode-%03o'%mode);p.write_bytes(b'permission probe')
            if not args.host:os.chown(p,owner,group)
            p.chmod(mode)
        if args.host:
            groups=os.getgroups();rows=probe(root,(os.getuid(),os.geteuid(),os.getgid(),os.getegid()),groups,owner,group,-1,-1,True);results['host-owner']=rows
        else:
            private=root/'private';private.mkdir();os.chown(private,A,G);private.chmod(0o700)
            f=private/'file';f.write_bytes(b'ancestor search control');f.chmod(0o777)
            f=private/'held';f.write_bytes(b'held');os.chown(f,A,G);f.chmod(0o460);held=os.open(f,os.O_RDONLY)
            f=root/'unlinked';f.write_bytes(b'unlinked');os.chown(f,A,G);f.chmod(0o640);unlinked=os.open(f,os.O_RDONLY);f.unlink()
            os.symlink('mode-000',root/'link')
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
                        rows=probe(root,(ru,eu,rg,eg),groups,owner,group,held,unlinked,False);data=json.dumps(rows).encode()
                        while data:data=data[os.write(write,data):]
                        os._exit(0)
                    except BaseException:__import__('traceback').print_exc();os._exit(1)
                os.close(write);data=b''
                while chunk:=os.read(read,65536):data+=chunk
                os.close(read);assert os.waitpid(pid,0)==(pid,0),(label,'child failure')
                results[label]=json.loads(data)
        mismatches=total=0
        for label,rows in results.items():
            bad=[r for r in rows if not r['matches']];mismatches+=len(bad);total+=len(rows)
            print('ACCESS_ACTOR',label,'cases',len(rows),'mismatches',len(bad),'examples',json.dumps(bad[:3]),flush=True)
        if args.output:Path(args.output).write_text(json.dumps(results,indent=2)+'\n')
        print('ACCESS_TOTAL',total,'MISMATCHES',mismatches,flush=True)
        assert mismatches>0 if args.record_baseline else mismatches==0
        print('ACCESS_BASELINE_RECORDED' if args.record_baseline else 'ACCESS_PERMISSIONS_PASS',flush=True)
    finally:
        if held>=0:os.close(held)
        if unlinked>=0:os.close(unlinked)
        cleanup(root);os.sync()


if __name__=='__main__':main()
