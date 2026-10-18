#!/usr/bin/env python3
"""Bounded real-kernel lookup/rename contention probe; not a proof of all races."""
import errno
import json
import os
from pathlib import Path
import re
import sys
import time


def cleanup(root):
    root.chmod(0o700)
    for p in root.iterdir():
        if p.is_dir() and not p.is_symlink(): cleanup(p)
        else: p.unlink()
    root.rmdir()


def main():
    assert os.geteuid()==0 and len(sys.argv)==2 and re.fullmatch('[0-9a-f]{16}',sys.argv[1])
    root=Path('/data')/('brook-lookup-'+sys.argv[1]);root.mkdir(mode=0o755);root.chmod(0o755)
    pid=None
    try:
        safe=root/'safe';safe.mkdir();safe.chmod(0o755);(safe/'file').write_bytes(b'public')
        private=root/'private';private.mkdir();private.chmod(0o700);(private/'file').write_bytes(b'secret')
        alias=root/'alias';os.symlink('safe',alias)
        read,write=os.pipe();pid=os.fork()
        if pid==0:
            os.close(read)
            try:
                os.setgroups([]);os.setgid(100);os.setuid(1000)
                counts={'public':0,'denied':0}
                for _ in range(400):
                    try:
                        data=(alias/'file').read_bytes()
                        assert data==b'public', 'protected bytes exposed'
                        counts['public']+=1
                    except OSError as e:
                        assert e.errno==errno.EACCES, e
                        counts['denied']+=1
                    time.sleep(.001)
                os.write(write,json.dumps(counts).encode());os._exit(0)
            except BaseException:
                __import__('traceback').print_exc();os._exit(1)
        os.close(write)
        for i in range(200):
            temporary=root/'next';os.symlink('safe' if i%2 else 'private',temporary)
            os.rename(temporary,alias);time.sleep(.002)
        data=b''
        while chunk:=os.read(read,4096):data+=chunk
        os.close(read);result=os.waitpid(pid,0);pid=None;assert result[1]==0,result
        counts=json.loads(data);assert sum(counts.values())==400
        print('LOOKUP_RENAME_CONTENTION_PASS',json.dumps(counts),flush=True)
    finally:
        if pid is not None:
            # Wait for this bounded child; never delete its fixture while live.
            os.waitpid(pid,0)
        cleanup(root);os.sync()


if __name__=='__main__':main()
