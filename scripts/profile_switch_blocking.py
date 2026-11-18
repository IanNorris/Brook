#!/usr/bin/env python3
"""Blocked time per pid by switch-out kernel stack, from a Brook profiler
capture (CS records, 1 ms ticks). Usage: profile_switch_blocking.py prof.txt kernel.elf [pid_hex,...]
Used to find the gen 427 epoll/mapping-mutex convoys (docs/xwayland-window-integration.md)."""
import sys,collections,subprocess
prof,elf=sys.argv[1],sys.argv[2]
want=sys.argv[3].split(',') if len(sys.argv)>3 else None
lastout={}
dur=collections.defaultdict(lambda: collections.defaultdict(lambda:[0,0,0]))
recs=[]
for line in open(prof):
    if not line.startswith('CS '):continue
    f=line.split()
    t=int(f[1]);old,new=f[3],f[4];st=f[5] if len(f)>5 else ''
    fr=st.split(';');key=';'.join(fr[2:5])
    lastout[old]=(t,key)
    if new in lastout:
        t0,k=lastout.pop(new);d=dur[new][k];d[0]+=1;d[1]+=t-t0;d[2]=max(d[2],t-t0)
addrs=set()
for p in dur:
    for k in dur[p]:addrs.update(a for a in k.split(';') if a)
addrs=sorted(addrs)
out=subprocess.run(['addr2line','-f','-C','-e',elf]+addrs,capture_output=True,text=True).stdout.split('\n')
names={a:out[2*i].split('(')[0].replace('brook::','')[:34] for i,a in enumerate(addrs)}
for p in (want or sorted(dur,key=lambda p:-sum(v[1] for v in dur[p].values()))[:12]):
    tot=sum(v[1] for v in dur[p].values())
    print(p,int(p,16),'blocked_total_ms',tot)
    for k,v in sorted(dur[p].items(),key=lambda kv:-kv[1][1])[:4]:
        print('   n=%d sum=%dms max=%dms mean=%.2f'%(v[0],v[1],v[2],v[1]/v[0]),' < '.join(names.get(a,a) for a in k.split(';') if a))
