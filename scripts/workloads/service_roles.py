#!/usr/bin/env python3
"""Owned guest fixture: keep a fork/exec + thread family alive for a role census.

Use a NEW directory directly under /data/home/regression-input/. The controller
creates ROOT/stop after recording identities, then checks ROOT/done.json and
full identity drain. A 120-second deadline releases the family if abandoned.
"""
import json
import os
from pathlib import Path
import sys
import threading
import time


def write(path, value):
    temporary=path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value)+'\n')
    temporary.replace(path)


def run(root, child=False):
    if root.parent != Path('/data/home/regression-input') or not root.name.startswith('role-fixture-'):
        raise ValueError('Use a new owned role-fixture directory in regression-input')
    if not child:root.mkdir(exist_ok=False)
    pid=None
    if not child:
        pid=os.fork()
        if not pid:
            os.execv('/nix/profile/bin/python3',['python3',__file__,str(root),'child'])
    release=threading.Event();ready=threading.Barrier(3);tids=[]
    def wait():
        tids.append(threading.get_native_id());ready.wait();release.wait(125)
    threads=[threading.Thread(target=wait) for _ in range(2)]
    for thread in threads:thread.start()
    ready.wait()
    phase='child' if child else 'parent'
    write(root/(phase+'.json'),{'pid':os.getpid(),'tids':sorted(tids),'uid':os.getuid(),
                               'euid':os.geteuid(),'gid':os.getgid(),'egid':os.getegid(),
                               'fork_child':pid,'after_exec':child})
    deadline=time.monotonic()+120
    while not (root/'stop').exists() and time.monotonic()<deadline:time.sleep(.05)
    timed_out=not (root/'stop').exists()
    if timed_out:(root/'stop').touch()
    release.set()
    for thread in threads:thread.join()
    status=0
    if pid:
        waited,status=os.waitpid(pid,0)
        assert waited==pid
    write(root/(phase+'-done.json'),{'pid':os.getpid(),'timeout':timed_out,'child_wait_status':status})
    if not child:write(root/'done.json',{'timeout':timed_out,'child_wait_status':status})
    return int(timed_out or status!=0)


if __name__=='__main__':
    if len(sys.argv) not in (2,3) or (len(sys.argv)==3 and sys.argv[2]!='child'):
        raise SystemExit('service_roles.py ROOT [child]')
    raise SystemExit(run(Path(sys.argv[1]),len(sys.argv)==3))
