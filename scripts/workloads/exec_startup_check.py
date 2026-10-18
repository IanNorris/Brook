#!/usr/bin/env python3
"""Bounded concurrent dynamic-ELF startup diagnostic on private review disks.

The selected executable must exit successfully without input. This exercises
exec's loader path, not kernel-queued boot creation or every return handler.
"""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import time

def main():
    if len(sys.argv)!=4:
        raise SystemExit('Usage: exec_startup_check.py executable workers batches')
    command=Path(sys.argv[1]).absolute()
    executable=command.resolve(strict=True)
    workers,batches=map(int,sys.argv[2:])
    assert 1<=workers<=32 and 1<=batches<=1000
    data=executable.read_bytes()
    assert data[:6]==b'\x7fELF\x02\x01', 'Requires a little-endian ELF64 executable'
    phoff=struct.unpack_from('<Q',data,32)[0]
    phsize,phnum=struct.unpack_from('<HH',data,54)
    assert phsize==56 and phoff+phsize*phnum<=len(data)
    interpreter=None
    for i in range(phnum):
        typ,_,offset,_,_,size,_,_=struct.unpack_from('<IIQQQQQQ',data,phoff+i*phsize)
        if typ==3:
            assert offset+size<=len(data)
            interpreter=data[offset:offset+size].rstrip(b'\0').decode()
    assert interpreter, 'A static executable cannot exercise interpreter startup'
    print('EXEC_STARTUP_IDENTITY',json.dumps({'command':str(command),'executable':str(executable),
          'sha256':hashlib.sha256(data).hexdigest(),'interpreter':interpreter,
          'interpreter_sha256':hashlib.sha256(Path(interpreter).read_bytes()).hexdigest()}),flush=True)
    started=time.monotonic();completed=0
    for batch in range(batches):
        children=[]
        failure=None
        try:
            for _ in range(workers):
                # Preserve argv[0]: true can be a symlink to a multicall
                # coreutils ELF whose behavior depends on the invoked name.
                children.append(subprocess.Popen([str(command)],stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE,stderr=subprocess.PIPE))
            for child in children:
                out,err=child.communicate(timeout=30)
                if child.returncode or out or err:
                    failure=f'pid={child.pid} exit={child.returncode} stdout={out!r} stderr={err!r}'
                else:completed+=1
        finally:
            # Only this probe's newly spawned children. Cleanup never turns a
            # timeout/failed launch into a successful diagnostic result.
            for child in children:
                if child.poll() is None:
                    child.kill();child.communicate(timeout=30)
        assert not failure,failure
        if batch%16==0:print('EXEC_BATCH_PASS',batch,'completed',completed,flush=True)
    assert completed==workers*batches
    print('EXEC_STARTUP_PASS',json.dumps({'workers':workers,'batches':batches,
          'completed':completed,'elapsed_s':time.monotonic()-started}),flush=True)

if __name__=='__main__':main()
