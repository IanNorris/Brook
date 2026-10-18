#!/usr/bin/env python3
"""Brook-specific relative/empty-path access and shared-Nix policy controls."""
import ctypes
import errno
import json
import os
from pathlib import Path
import re
import sys

libc=ctypes.CDLL(None,use_errno=True);libc.syscall.restype=ctypes.c_long
EMPTY=0x1000


def access(fd,path,mode,flags=0):
    result=libc.syscall(ctypes.c_long(439),ctypes.c_int(fd),ctypes.c_char_p(os.fsencode(path)),ctypes.c_int(mode),ctypes.c_int(flags))
    return ctypes.get_errno() if result<0 else 0


def cleanup(p):
    p.chmod(0o700)
    for child in p.iterdir():
        if child.is_dir() and not child.is_symlink():cleanup(child)
        else:child.unlink()
    p.rmdir()


def main():
    assert os.geteuid()==0 and len(sys.argv)==2 and re.fullmatch('[0-9a-f]{16}',sys.argv[1])
    data=Path('/data')/('brook-accessflags-'+sys.argv[1]);nix=Path('/nix')/('brook-accessflags-'+sys.argv[1]);fds=[]
    data.mkdir(mode=0o755);nix.mkdir(mode=0o755)
    try:
        owned=data/'owned';owned.write_bytes(b'owned');os.chown(owned,1000,100);owned.chmod(0o400)
        cwd=data/'cwd';cwd.mkdir();os.chown(cwd,1000,100);cwd.chmod(0o500)
        shared=nix/'mode-zero';shared.write_bytes(b'shared policy');shared.chmod(0)
        directory=os.open(data,os.O_RDONLY|os.O_DIRECTORY);fds.append(directory)
        regular=os.open(owned,os.O_RDONLY);fds.append(regular)
        sharedfd=os.open(shared,os.O_RDONLY);fds.append(sharedfd)
        pipe_read,pipe_write=os.pipe();fds.extend([pipe_read,pipe_write])
        read,write=os.pipe();pid=os.fork()
        if pid==0:
            os.close(read)
            try:
                os.setgroups([]);os.setgid(100);os.setuid(1000);rows=[]
                def check(label,actual,expected):rows.append(dict(case=label,actual=actual,expected=expected,matches=actual==expected))
                for mode in range(8):
                    expected=0 if mode&4==mode else errno.EACCES
                    check('relative-'+str(mode),access(directory,'owned',mode),expected)
                    check('retained-'+str(mode),access(regular,'',mode,EMPTY),expected)
                    check('nix-path-'+str(mode),access(-100,shared,mode),0)
                    check('nix-fd-'+str(mode),access(sharedfd,'',mode,EMPTY),0)
                check('relative-bad-fd',access(-1,'owned',0),errno.EBADF)
                check('relative-file-fd',access(regular,'owned',0),errno.ENOTDIR)
                check('pipe-existence',access(pipe_read,'',0,EMPTY),0)
                check('pipe-read-unsupported',access(pipe_read,'',4,EMPTY),errno.EOPNOTSUPP)
                check('pipe-write-unsupported',access(pipe_write,'',2,EMPTY),errno.EOPNOTSUPP)
                os.chdir(cwd)
                for mode in range(8):check('empty-cwd-'+str(mode),access(-100,'',mode,EMPTY),0 if mode&5==mode else errno.EACCES)
                payload=json.dumps(rows).encode()
                while payload:payload=payload[os.write(write,payload):]
                os._exit(0)
            except BaseException:__import__('traceback').print_exc();os._exit(1)
        os.close(write);payload=b''
        while chunk:=os.read(read,65536):payload+=chunk
        os.close(read);assert os.waitpid(pid,0)==(pid,0)
        rows=json.loads(payload)
        for row in rows:print(json.dumps(row),flush=True)
        assert all(x['matches'] for x in rows)
        print('ACCESS_FLAGS_PASS',len(rows),flush=True)
    finally:
        for fd in fds:os.close(fd)
        cleanup(data);cleanup(nix);os.sync()


if __name__=='__main__':main()
