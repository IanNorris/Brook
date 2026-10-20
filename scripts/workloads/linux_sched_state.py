#!/usr/bin/env python3
"""Read Linux x86-64 scheduler requests/configuration without changing settings.

Default: current thread only. --all-user-tasks is intended for the disposable
Linux benchmark guest; reports thread names/policies/slices, never command lines.
Missing privileged debugfs/config data is recorded rather than assumed.
"""
import argparse
import collections
import ctypes
import gzip
import json
import os
from pathlib import Path
import platform


class SchedAttr(ctypes.Structure):
    _fields_=[('size',ctypes.c_uint32),('policy',ctypes.c_uint32),
              ('flags',ctypes.c_uint64),('nice',ctypes.c_int32),
              ('priority',ctypes.c_uint32),('runtime_ns',ctypes.c_uint64),
              ('deadline_ns',ctypes.c_uint64),('period_ns',ctypes.c_uint64),
              ('util_min',ctypes.c_uint32),('util_max',ctypes.c_uint32)]


def attr(tid):
    if platform.machine()!='x86_64' or ctypes.sizeof(SchedAttr)!=56:
        raise RuntimeError('This read-only probe requires Linux x86-64 sched_attr ABI')
    libc=ctypes.CDLL(None,use_errno=True);libc.syscall.restype=ctypes.c_long
    value=SchedAttr();value.size=ctypes.sizeof(value)
    if libc.syscall(ctypes.c_long(315),ctypes.c_int(tid),ctypes.byref(value),ctypes.c_uint(value.size),ctypes.c_uint(0)):
        return dict(tid=tid,error=os.strerror(ctypes.get_errno()))
    return dict(tid=tid,**{name:getattr(value,name) for name,_ in value._fields_})


def read(path):
    try:return dict(value=Path(path).read_text().strip())
    except OSError as e:return dict(error=str(e))


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--all-user-tasks',action='store_true');p.add_argument('--output',type=Path)
    a=p.parse_args();tasks=[]
    if a.all_user_tasks:
        for proc in sorted(Path('/proc').glob('[0-9]*')):
            try:
                if proc.stat().st_uid!=os.getuid():continue
                candidates=list((proc/'task').iterdir())
            except OSError:continue
            for task in candidates:
                v=attr(int(task.name));v.update(tgid=int(proc.name),name=read(task/'comm'),cgroup=read(task/'cgroup'),autogroup=read(proc/'autogroup'));tasks.append(v)
                if len(tasks)>4096:raise RuntimeError('Task inventory exceeded bounded probe limit')
    else:tasks=[dict(**attr(0),tgid=os.getpid(),name=read('/proc/self/comm'),cgroup=read('/proc/self/cgroup'),autogroup=read('/proc/self/autogroup'))]
    settings={path:read(path) for path in ('/proc/cmdline','/sys/devices/system/cpu/online','/sys/devices/system/clocksource/clocksource0/current_clocksource','/sys/kernel/debug/sched/base_slice_ns','/sys/kernel/debug/sched/tunable_scaling','/sys/kernel/debug/sched/preempt','/sys/kernel/debug/sched/features','/proc/sys/kernel/sched_autogroup_enabled','/proc/sys/kernel/sched_schedstats')}
    try:
        with gzip.open('/proc/config.gz','rt') as f:lines=f.read(2000000).splitlines()
        keys=('CONFIG_HZ=','CONFIG_PREEMPT','CONFIG_NO_HZ','CONFIG_SCHED_DEBUG=','CONFIG_SCHED_AUTOGROUP=','CONFIG_FAIR_GROUP_SCHED=','CONFIG_SCHED_HRTICK=','CONFIG_HIGH_RES_TIMERS=')
        config=[x for x in lines if x.startswith(keys)]
    except OSError as e:config=dict(error=str(e))
    counts=collections.Counter((x['policy'],x['nice'],x['runtime_ns']) for x in tasks if 'error' not in x)
    result=dict(kernel=platform.release(),machine=platform.machine(),uid=os.getuid(),settings=settings,selected_kernel_config=config,tasks=tasks,slice_counts=[dict(policy=k[0],nice=k[1],runtime_ns=k[2],threads=v) for k,v in sorted(counts.items())],
        limits='sched_getattr reports per-thread requested slices for fair tasks, not uninterrupted CPU residence. Inventory races with task exit/rename. Missing root-only settings remain unknown. This probe never sets scheduler parameters.')
    text=json.dumps(result,indent=2)+'\n'
    if a.output:a.output.write_text(text);print(json.dumps(dict(kernel=result['kernel'],tasks=len(tasks),slice_counts=result['slice_counts'])))
    else:print(text,end='')


if __name__=='__main__':main()
