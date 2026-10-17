#!/usr/bin/env python3
"""Record remaining path authorization gaps in private guest fixtures.

Root-only guest audit; this deliberately reports incomplete behavior rather than
calling expected failures acceptance. Parent credentials remain unchanged.
Argument: output JSON path. No host files outside the private /data fixture.
"""
import errno
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile


def main():
    assert os.geteuid()==0,'Run inside the isolated Brook test guest as root.'
    directory=Path(tempfile.mkdtemp(prefix='brook-path-audit-',dir='/data'))
    os.chmod(directory,0o755)
    private=directory/'private'; public=directory/'public'; ro=directory/'readonly'; sticky=directory/'sticky'
    try:
        for path,mode in [(private,0o700),(public,0o777),(ro,0o555),(sticky,0o1777)]:
            path.mkdir(); os.chmod(path,mode)
        os.chown(private,1000,200)
        (private/'deeper').mkdir()
        for path in [private/'payload',private/'marker',public/'marker',public/'file',
                     ro/'unlink-me',ro/'rename-source',public/'rename-source',
                     sticky/'unlink-me',sticky/'rename-source']:
            path.write_bytes(b'private audit fixture')
            os.chmod(path,0o666)
        for path in [sticky/'unlink-me',sticky/'rename-source']:
            os.chown(path,1000,200)
        (ro/'empty').mkdir()
        os.symlink('payload',private/'link')
        os.symlink('../private/deeper',public/'jump')
        os.symlink(str(private/'payload'),public/'absolute-link')
        r,w=os.pipe()
        pid=os.fork()
        if pid==0:
            os.close(r)
            rows=[]
            try:
                os.setgroups([]); os.setresgid(100,100,100); os.setresuid(1001,1001,1001)
                def opened(path,flags=os.O_RDONLY):
                    fd=os.open(path,flags,0o600); os.close(fd)
                actions=[('private-open',errno.EACCES,lambda:opened(private/'payload')),
                         ('private-stat',errno.EACCES,lambda:os.stat(private/'payload')),
                         ('private-readlink',errno.EACCES,lambda:os.readlink(private/'link')),
                         ('locked-dotdot',errno.EACCES,lambda:opened(str(private)+'/../public/marker')),
                         ('file-dotdot',errno.ENOTDIR,lambda:opened(str(public/'file')+'/../marker')),
                         ('symlink-before-dotdot',errno.EACCES,lambda:opened(str(public/'jump')+'/../marker')),
                         ('absolute-symlink-search',errno.EACCES,lambda:opened(public/'absolute-link')),
                         ('readonly-create',errno.EACCES,lambda:opened(ro/'created',os.O_CREAT|os.O_RDWR)),
                         ('readonly-mkdir',errno.EACCES,lambda:os.mkdir(ro/'created-dir')),
                         ('readonly-symlink',errno.EACCES,lambda:os.symlink('x',ro/'created-link')),
                         ('readonly-unlink',errno.EACCES,lambda:os.unlink(ro/'unlink-me')),
                         ('readonly-rmdir',errno.EACCES,lambda:os.rmdir(ro/'empty')),
                         ('rename-source-parent',errno.EACCES,lambda:os.rename(ro/'rename-source',public/'moved-from-ro')),
                         ('rename-dest-parent',errno.EACCES,lambda:os.rename(public/'rename-source',ro/'moved-into-ro')),
                         ('sticky-unlink',errno.EPERM,lambda:os.unlink(sticky/'unlink-me')),
                         ('sticky-rename',errno.EPERM,lambda:os.rename(sticky/'rename-source',public/'moved-sticky')),
                         ('public-create-control',0,lambda:opened(public/'created',os.O_CREAT|os.O_RDWR))]
                for name,expected,action in actions:
                    try:action();actual=0
                    except OSError as exc:actual=exc.errno
                    rows.append(dict(name=name,expected=expected,actual=actual,matches=actual==expected))
                actual=os.access(private/'payload',os.R_OK)
                rows.append(dict(name='access-search',expected=False,actual=actual,matches=not actual))
                report=dict(identity=dict(uid=os.getresuid(),gid=os.getresgid(),groups=os.getgroups()),rows=rows)
            except BaseException as exc:
                report=dict(error=repr(exc),rows=rows)
            data=json.dumps(report).encode()
            while data:data=data[os.write(w,data):]
            os.close(w);os._exit(0)
        os.close(w);data=bytearray()
        while chunk:=os.read(r,65536):data.extend(chunk)
        os.close(r);_,status=os.waitpid(pid,0)
        assert status==0,status
        report=json.loads(data)
        Path(sys.argv[1]).write_text(json.dumps(report,indent=2)+'\n')
        for row in report['rows']:print('PATH_AUDIT',json.dumps(row),flush=True)
        print('PATH_AUDIT_REMAINING_GAPS',sum(not row['matches'] for row in report['rows']),flush=True)
        assert 'error' not in report,report
    finally:
        shutil.rmtree(directory)

if __name__=='__main__':main()
