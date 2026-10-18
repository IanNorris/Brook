#!/usr/bin/env python3
"""Component order/search regression. Host owner controls or root guest children.

No account-isolation claim: dirfd/CWD are still retained strings, and virtual
filesystems plus real-ID access() and leaf execute authorization need more work.
"""
import argparse
import errno
import json
import os
from pathlib import Path
import re
import stat


def cleanup(directory):
    directory.chmod(0o700)
    for path in directory.iterdir():
        if path.is_symlink(): path.unlink()
        elif path.is_dir(): cleanup(path)
        else: path.unlink()
    directory.rmdir()


def run(root, fd, guest, nixroot):
    public=root/'public';locked=root/'locked';real=public/'real';rows=[]
    def error(name, expected, operation):
        try:operation();actual=0
        except OSError as e:actual=e.errno
        rows.append(dict(case=name,actual=actual,expected=expected,matches=actual==expected))
    def value(name, expected, operation):
        actual=operation();rows.append(dict(case=name,actual=actual,expected=expected,matches=actual==expected))
    def opened(p, flags=os.O_RDONLY):
        f=os.open(p,flags,0o600);os.close(f)
    error('locked-open',errno.EACCES,lambda:opened(locked/'file'))
    error('locked-stat',errno.EACCES,lambda:os.stat(locked/'file'))
    error('locked-lstat',errno.EACCES,lambda:os.lstat(locked/'link'))
    error('locked-readlink',errno.EACCES,lambda:os.readlink(locked/'link'))
    error('locked-dot',errno.EACCES,lambda:os.stat(str(locked)+'/.'))
    error('locked-dotdot',errno.EACCES,lambda:opened(str(locked)+'/../public/file'))
    error('locked-create-dotdot',errno.EACCES,lambda:opened(str(locked)+'/../public/denied',os.O_CREAT|os.O_WRONLY))
    error('file-dotdot',errno.ENOTDIR,lambda:opened(str(public/'file')+'/../file'))
    error('file-slash',errno.ENOTDIR,lambda:opened(str(public/'file')+'/'))
    error('symlink-before-dotdot',errno.EACCES,lambda:opened(str(public/'jump')+'/../file'))
    error('absolute-link-search',errno.EACCES,lambda:opened(public/'absolute'))
    error('missing-middle',errno.ENOENT,lambda:opened(public/'absent'/'file'))
    error('loop-stat',errno.ELOOP,lambda:os.stat(root/'loop'))
    error('loop-open',errno.ELOOP,lambda:opened(root/'loop'))
    error('locked-chmod',errno.EACCES,lambda:os.chmod(locked/'file',0o600))
    error('locked-times',errno.EACCES,lambda:os.utime(locked/'file',None))
    value('access-search',False,lambda:os.access(locked/'file',os.F_OK))
    value('lstat-leaf-no-search',True,lambda:os.lstat(locked).st_ino!=0)
    value('lstat-leaf-slash',True,lambda:stat.S_ISDIR(os.lstat(str(locked)+'/').st_mode))
    value('relative-link', 'payload',lambda:(public/'good-file').read_text())
    value('lstat-symlink',True,lambda:stat.S_ISLNK(os.lstat(public/'good-dir').st_mode))
    value('lstat-symlink-slash',True,lambda:stat.S_ISDIR(os.lstat(str(public/'good-dir')+'/').st_mode))
    error('unlink-symlink-slash',errno.ENOTDIR,lambda:os.unlink(str(public/'good-dir')+'/'))
    error('rename-symlink-slash',errno.ENOTDIR,lambda:os.rename(str(public/'good-dir')+'/',public/'renamed-dir'))
    error('missing-create-slash',errno.EISDIR,lambda:opened(str(public/'missing')+'/',os.O_CREAT|os.O_WRONLY))
    error('file-mkdir-slash',errno.EEXIST,lambda:os.mkdir(str(public/'file')+'/'))
    error('file-mkdir-dot',errno.ENOTDIR,lambda:os.mkdir(str(public/'file')+'/.'))
    error('fchdir-search',errno.EACCES,lambda:os.fchdir(fd))
    old=os.getcwd()
    try:
        error('chdir-locked',errno.EACCES,lambda:os.chdir(locked))
        os.chdir(public/'good-dir');value('physical-cwd',str(real),os.getcwd)
        os.chdir('..');value('physical-cwd-parent',str(public),os.getcwd)
    finally:os.chdir(old)
    alias=root/'public-alias'
    error('create-through-parent-link',0,lambda:opened(alias/'created',os.O_CREAT|os.O_EXCL|os.O_WRONLY))
    value('created-in-target',True,lambda:(public/'created').exists())
    error('rename-through-parent-link',0,lambda:os.rename(alias/'created',alias/'renamed'))
    value('renamed-in-target',True,lambda:(public/'renamed').exists())
    error('unlink-through-parent-link',0,lambda:os.unlink(alias/'renamed'))
    error('mkdir-through-parent-link',0,lambda:os.mkdir(alias/'new-dir'))
    error('rmdir-through-parent-link',0,lambda:os.rmdir(alias/'new-dir'))
    if guest:
        regular=os.open(public/'file',os.O_RDONLY)
        try:
            fdpath='/proc/self/fd/'+str(regular)
            error('proc-fd-stat-compatibility',0,lambda:os.stat(fdpath))
            value('proc-fd-lstat-link',True,lambda:stat.S_ISLNK(os.lstat(fdpath).st_mode))
            value('proc-fd-readlink-compatibility',True,lambda:bool(os.readlink(fdpath)))
        finally:os.close(regular)
        value('cross-mount-target-before-dotdot','nix-marker',lambda:Path(str(public/'cross')+'/../marker').read_text())
        value('mount-root-dotdot','payload',lambda:Path('/nix/../'+str(public/'file').lstrip('/')).read_text())
    return rows


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--host',action='store_true');ap.add_argument('--root',default='/data');ap.add_argument('--case',required=True)
    args=ap.parse_args();assert re.fullmatch('[0-9a-f]{16}',args.case)
    assert args.host or os.geteuid()==0
    root=Path(args.root)/('brook-walk-'+args.case);root.mkdir(mode=0o755);root.chmod(0o755)
    nixroot=None;fd=-1
    try:
        public=root/'public';public.mkdir(mode=0o777);public.chmod(0o777)
        real=public/'real';real.mkdir();(public/'file').write_text('payload')
        locked=root/'locked';locked.mkdir();(locked/'file').write_text('private');(locked/'deep').mkdir()
        os.symlink('file',locked/'link');os.symlink('../locked/deep',public/'jump')
        os.symlink(str(locked/'file'),public/'absolute');os.symlink('file',public/'good-file')
        os.symlink('real',public/'good-dir');os.symlink('public',root/'public-alias');os.symlink('loop',root/'loop')
        fd=os.open(locked,os.O_RDONLY|os.O_DIRECTORY);locked.chmod(0)
        if not args.host:
            nixroot=Path('/nix')/('brook-walk-'+args.case);nixroot.mkdir();(nixroot/'deep').mkdir();(nixroot/'marker').write_text('nix-marker')
            os.symlink(str(nixroot/'deep'),public/'cross')
            read,write=os.pipe();pid=os.fork()
            if pid==0:
                os.close(read)
                try:
                    os.setgroups([]);os.setgid(100);os.setuid(1000)
                    data=json.dumps(run(root,fd,True,nixroot)).encode()
                    while data:data=data[os.write(write,data):]
                    os._exit(0)
                except BaseException:
                    __import__('traceback').print_exc();os._exit(1)
            os.close(write);data=b''
            while chunk:=os.read(read,65536):data+=chunk
            os.close(read);assert os.waitpid(pid,0)==(pid,0)
            rows=json.loads(data)
        else:rows=run(root,fd,False,None)
        for row in rows:print(json.dumps(row),flush=True)
        failures=[r['case'] for r in rows if not r['matches']]
        print('PATH_TRAVERSAL_FAILURES',json.dumps(failures),flush=True);assert not failures,failures
        print('PATH_TRAVERSAL_PASS',len(rows),flush=True)
    finally:
        if fd>=0:os.close(fd)
        cleanup(root)
        if nixroot:cleanup(nixroot)
        os.sync()


if __name__=='__main__':main()
