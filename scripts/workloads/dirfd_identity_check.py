#!/usr/bin/env python3
"""Owned retained-directory workload probe for Linux and Brook.

Exercises long paths, canonical symlink opens, dup/fork retention, sequential
rename/replacement and fd-safe recursive cleanup. It does not prove atomic *at
resolution under concurrent namespace mutation or pinned process CWD.
"""
import errno
import os
from pathlib import Path
import shutil
import sys
import tempfile
import time

parent=Path(sys.argv[1] if len(sys.argv)>1 else tempfile.gettempdir())
root=Path(tempfile.mkdtemp(prefix='brook-dirfd-',dir=parent))
checks=0

def check(condition,label):
    global checks
    checks+=1
    if not condition:raise AssertionError(label)

def read_at(fd,name):
    handle=os.open(name,os.O_RDONLY,dir_fd=fd)
    try:return os.read(handle,4096)
    finally:os.close(handle)

def fails(expected,call,label):
    try:call()
    except OSError as error:check(error.errno==expected,(label,error))
    else:check(False,label+' unexpectedly succeeded')

# Absolute cleanup is only an error-path fallback. Success must pass the actual
# fd-based shutil.rmtree path that exposed Brook's truncation.
def fallback_cleanup():
    for base,directories,files in os.walk(root,topdown=False):
        for name in files:(Path(base)/name).unlink()
        for name in directories:
            path=Path(base)/name
            if path.is_symlink():path.unlink()
            else:path.rmdir()
    root.rmdir()

started=time.monotonic()
try:
    for wanted in [63,64,127,230]:
        boundary=root/('b'*(wanted-len(str(root))-1))
        boundary.mkdir();(boundary/'payload').write_bytes(b'boundary identity')
        fd=os.open(boundary,os.O_RDONLY|os.O_DIRECTORY)
        try:
            check(len(str(boundary))==wanted,'exact path-length boundary')
            check(read_at(fd,'payload')==b'boundary identity','boundary openat')
            check(os.stat('payload',dir_fd=fd).st_ino==(boundary/'payload').stat().st_ino,'boundary statat')
            os.unlink('payload',dir_fd=fd)
            check(not (boundary/'payload').exists(),'boundary unlinkat')
        finally:os.close(fd)
        boundary.rmdir()
    deep=root/('long-directory-component-'*3)
    deep.mkdir();(deep/'payload').write_bytes(b'original directory identity')
    (deep/'alias-file').symlink_to('payload')
    (root/'alias-dir').symlink_to(deep.name)
    check(len(str(deep))>64,'fixture must exceed former descriptor prefix')
    for opening in [deep,root/'alias-dir']:
        fd=os.open(opening,os.O_RDONLY|os.O_DIRECTORY)
        duplicate=os.dup(fd)
        os.close(fd);fd=duplicate
        try:
            check(read_at(fd,'payload')==b'original directory identity','openat long/canonical/dup')
            st=os.stat('payload',dir_fd=fd)
            check((st.st_dev,st.st_ino)==((deep/'payload').stat().st_dev,(deep/'payload').stat().st_ino),'statat identity')
            check(os.stat('alias-file',dir_fd=fd,follow_symlinks=False).st_mode&0o170000==0o120000,'lstatat symlink')
            check(os.readlink('alias-file',dir_fd=fd)=='payload','readlinkat')
            check(os.access('payload',os.R_OK,dir_fd=fd),'faccessat long prefix')
            os.mkdir('created',dir_fd=fd);check((deep/'created').is_dir(),'mkdirat')
            child=os.open('created/file',os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600,dir_fd=fd)
            os.write(child,b'created through directory fd');os.close(child)
            os.rename('created/file','created/renamed',src_dir_fd=fd,dst_dir_fd=fd)
            check(read_at(fd,'created/renamed')==b'created through directory fd','renameat')
            os.unlink('created/renamed',dir_fd=fd);os.rmdir('created',dir_fd=fd)
            check(not (deep/'created').exists(),'unlinkat and rmdir')
            cwd=os.open('.',os.O_RDONLY|os.O_DIRECTORY)
            try:
                os.fchdir(fd);check(Path.cwd()==deep,'fchdir long canonical directory')
            finally:os.fchdir(cwd);os.close(cwd)
            pid=os.fork()
            if pid==0:
                try:os._exit(0 if read_at(fd,'payload')==b'original directory identity' else 95)
                except BaseException:os._exit(96)
            waited,status=os.waitpid(pid,0);check(waited==pid and status==0,'fork retains directory')
        finally:os.close(fd)
    # Move the retained directory and put a different directory at its old name.
    # Relative operations must keep addressing the original retained inode.
    fd=os.open(deep,os.O_RDONLY|os.O_DIRECTORY)
    moved=root/('moved-directory-component-'*3)
    try:
        deep.rename(moved);deep.mkdir();(deep/'payload').write_bytes(b'replacement must not be read')
        check(read_at(fd,'payload')==b'original directory identity','rename/replacement retains inode')
        os.mkdir('after-move',dir_fd=fd)
        check((moved/'after-move').is_dir() and not (deep/'after-move').exists(),'mutation after rename')
        os.rmdir('after-move',dir_fd=fd)
        cwd=os.open('.',os.O_RDONLY|os.O_DIRECTORY)
        try:os.fchdir(fd);check(Path.cwd()==moved,'fchdir follows retained moved directory')
        finally:os.fchdir(cwd);os.close(cwd)
    finally:os.close(fd)
    # A detached directory must never start addressing a replacement by name.
    detached=root/'detached';detached.mkdir();fd=os.open(detached,os.O_RDONLY|os.O_DIRECTORY)
    try:
        detached.rmdir();detached.mkdir();(detached/'payload').write_bytes(b'wrong inode')
        fails(errno.ENOENT,lambda:read_at(fd,'payload'),'unlinked directory replacement')
    finally:os.close(fd)
    shutil.rmtree(root)
    check(not root.exists(),'fd-safe recursive cleanup')
    print(f'DIRFD_IDENTITY_PASS checks={checks} uid={os.getuid()} seconds={time.monotonic()-started:.3f}',flush=True)
except BaseException:
    if root.exists():fallback_cleanup()
    raise
