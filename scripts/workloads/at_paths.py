#!/usr/bin/env python3
"""Descriptor-relative syscall errors in a private fixture (no root required).

Arguments: result JSON path, then 'brook' for Brook's documented path-size and
unsupported rename-policy checks. Host Linux runs the portable error cases.
"""
import ctypes
import errno
import json
import os
from pathlib import Path
import sys
import tempfile

libc = ctypes.CDLL(None, use_errno=True)
libc.syscall.restype = ctypes.c_long
brook = len(sys.argv)>2 and sys.argv[2]=='brook'
results = []


def raw(number, *args):
    converted = [ctypes.c_char_p(a.encode()) if isinstance(a,str) else
                 ctypes.c_long(a) if isinstance(a,int) else a for a in args]
    ctypes.set_errno(0)
    result = libc.syscall(ctypes.c_long(number),*converted)
    if result == -1:
        raise OSError(ctypes.get_errno(),'syscall %d'%number)
    return result


def check(name, expected, action):
    try:
        action()
        actual = 0
    except OSError as exc:
        actual = exc.errno
    row = dict(name=name,expected=expected,actual=actual,passed=actual==expected)
    results.append(row)
    print('AT_PATH_CASE',json.dumps(row),flush=True)


def main():
    output = Path(sys.argv[1]).absolute() if len(sys.argv)>1 else None
    cwd = os.getcwd()
    with tempfile.TemporaryDirectory(prefix='brook-at-',dir='/data' if brook else '/tmp') as name:
        directory = Path(name)
        os.chdir(directory)
        Path('present').write_bytes(b'present stays private')
        os.symlink('present','link')
        fd = os.open('present',os.O_RDONLY)
        dfd = os.open('.',os.O_RDONLY|os.O_DIRECTORY)
        try:
            statbuf=ctypes.create_string_buffer(256)
            linkbuf=ctypes.create_string_buffer(256)
            for label,base,expected in [('bad-fd',-1,errno.EBADF),('file-fd',fd,errno.ENOTDIR)]:
                def opened(path,base=base):
                    os.close(raw(257,base,path,os.O_RDONLY,0))
                calls=[('open',lambda:opened('present')),
                       ('stat',lambda:raw(262,base,'present',statbuf,0)),
                       ('statx',lambda:raw(332,base,'present',0,0x7ff,statbuf)),
                       ('mkdir',lambda:raw(258,base,'created-'+label,0o700)),
                       ('unlink',lambda:raw(263,base,'present',0)),
                       ('readlink',lambda:raw(267,base,'link',linkbuf,256)),
                       ('symlink',lambda:raw(266,'present',base,'newlink-'+label)),
                       ('chmod',lambda:raw(268,base,'present',0o600)),
                       ('utime',lambda:raw(280,base,'present',0,0))]
                for op,action in calls:
                    # Restore sentinel if a faulty unlink reaches CWD.
                    if not Path('present').exists():Path('present').write_bytes(b'present stays private')
                    check(label+'-'+op,expected,action)
                for side in ('source','destination'):
                    Path('source').write_bytes(b'source')
                    Path('destination').write_bytes(b'destination')
                    oldfd=base if side=='source' else -100
                    newfd=base if side=='destination' else -100
                    check(label+'-rename-'+side,expected,
                          lambda:raw(264,oldfd,'source',newfd,'destination'))
                    if Path('destination').read_bytes()!=b'destination':
                        results[-1]['passed']=False
                        results[-1]['mutation']='destination replaced'
            check('absolute-ignores-bad-fd',0,
                  lambda:os.close(raw(257,-1,str(directory/'present'),os.O_RDONLY,0)))
            check('valid-directory-open',0,
                  lambda:os.close(raw(257,dfd,'present',os.O_RDONLY,0)))
            check('valid-directory-stat',0,lambda:raw(262,dfd,'present',statbuf,0))
            check('valid-directory-statx',0,lambda:raw(332,dfd,'present',0,0x7ff,statbuf))
            check('valid-directory-mkdir',0,lambda:raw(258,dfd,'valid-created',0o700))
            check('valid-directory-rmdir',0,lambda:raw(263,dfd,'valid-created',0x200))
            check('valid-directory-symlink',0,lambda:raw(266,'present',dfd,'valid-link'))
            def readlink(base,path):
                length=raw(267,base,path,linkbuf,256)
                assert linkbuf.raw[:length]==b'present'
            check('valid-directory-readlink',0,lambda:readlink(dfd,'valid-link'))
            check('valid-cwd-readlink',0,lambda:readlink(-100,'valid-link'))
            check('valid-directory-unlink',0,lambda:raw(263,dfd,'valid-link',0))
            check('valid-directory-chmod',0,lambda:raw(268,dfd,'present',0o600))
            assert os.stat('present').st_mode & 0o777 == 0o600
            check('valid-directory-utime',0,lambda:raw(280,dfd,'present',0,0))
            for number in (264,316):
                Path('source').write_bytes(b'source')
                check('valid-directory-rename-%d'%number,0,
                      lambda:raw(number,dfd,'source',dfd,'moved-%d'%number,0))
            check('empty-open',errno.ENOENT,lambda:os.close(raw(257,-100,'',os.O_RDONLY,0)))
            Path('source').write_bytes(b'source')
            Path('destination').write_bytes(b'destination')
            check('rename-unknown-flags',errno.EINVAL,
                  lambda:raw(316,-100,'source',-100,'destination',0x80000000))
            if Path('destination').read_bytes()!=b'destination':
                results[-1].update(passed=False,mutation='unknown flags replaced target')
            Path('source').write_bytes(b'source')
            Path('destination').write_bytes(b'destination')
            check('rename-no-replace',errno.EINVAL if brook else errno.EEXIST,
                  lambda:raw(316,-100,'source',-100,'destination',1))
            if Path('destination').read_bytes()!=b'destination':
                results[-1].update(passed=False,mutation='no-replace replaced target')
            if brook:
                longdir=directory/('d'*100)
                longdir.mkdir()
                os.chdir(longdir)
                check('cwd-join-does-not-truncate',errno.ENAMETOOLONG,
                      lambda:os.close(raw(257,-100,'n'*150,os.O_CREAT|os.O_RDWR,0o600)))
                unexpected=list(longdir.iterdir())
                if unexpected:
                    results[-1].update(passed=False,mutation='truncated join created a different file')
                    for path in unexpected:os.unlink(str(path))
        finally:
            os.close(fd)
            os.close(dfd)
            os.chdir(cwd)
    if output:output.write_text(json.dumps(results,indent=2)+'\n')
    assert all(row['passed'] for row in results),'descriptor-relative syscall mismatch'
    print('AT_PATH_PASS',len(results),flush=True)

if __name__=='__main__':main()
