#!/usr/bin/env python3
"""Check earlier timer rearm through each multiplexing API and sub-ms periods.

The loose latency bound catches a lost rearm that waits for the old 2s deadline;
it is not a scheduler latency benchmark or deterministic lock-race injection.
"""
import os,select,time,threading,json,ctypes as c
l=c.CDLL(None,use_errno=True)
class T(c.Structure):_fields_=[('s',c.c_long),('n',c.c_long)]
class I(c.Structure):_fields_=[('i',T),('v',T)]
def arm(fd,ns,interval=0):
 x=I(T(*divmod(interval,10**9)),T(*divmod(ns,10**9)));assert l.timerfd_settime(fd,0,c.byref(x),None)==0
rows=[]
for method in ('poll','select','epoll','nested'):
 fd=l.timerfd_create(1,os.O_NONBLOCK);assert fd>=0;arm(fd,2*10**9);result=[]
 def wait():
  ep=outer=None
  try:
   if method=='poll':
    p=select.poll();p.register(fd,select.POLLIN);r=p.poll(3000)
   elif method=='select':r=select.select([fd],[],[],3)[0]
   else:
    ep=select.epoll();ep.register(fd,select.EPOLLIN)
    if method=='nested':outer=select.epoll();outer.register(ep.fileno(),select.EPOLLIN);r=outer.poll(3)
    else:r=ep.poll(3)
   result.append((bool(r),time.monotonic_ns()))
  finally:
   if outer is not None:outer.close()
   if ep is not None:ep.close()
 t=threading.Thread(target=wait,daemon=True);t.start();time.sleep(.03);start=time.monotonic_ns();arm(fd,16000000);t.join(4)
 assert result and result[0][0],(method,result)
 delay=(result[0][1]-start)/1e6;assert delay<500,(method,delay)
 assert int.from_bytes(os.read(fd,8),'little')==1;rows.append({'method':method,'rearm_ms':delay})
 arm(fd,500000,500000);time.sleep(.02);count=int.from_bytes(os.read(fd,8),'little');assert count>=20,(method,count);os.close(fd)
print(json.dumps(rows),flush=True);print('TIMERFD_REARM_READINESS_PASS',flush=True)
