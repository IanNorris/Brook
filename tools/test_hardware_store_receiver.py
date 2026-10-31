#!/usr/bin/env python3
"""Loopback receiver checks with disposable ext2 images; never uses real USB."""
import hashlib,json,os,socket,struct,subprocess,sys,tempfile,uuid
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from hardware_bulk_protocol import line
from hardware_store_protocol import provision

def reply(conn):
 try: return line(conn)
 except RuntimeError as error: return error.args[0]

image=Path(sys.argv[1]).resolve(strict=True);binary=Path(sys.argv[2]).resolve(strict=True)
record=b'BRKSTORE1 16777216 01234567-89ab-cdef-0123-456789abcdef'
assert image.stat().st_size==16777216
payload=image.read_bytes();digest=hashlib.sha256(payload).digest();key='1'*32;boot='2'*32
with tempfile.TemporaryDirectory(prefix='brook-store-receiver-',dir='build/hardware') as temp:
 root=Path(temp).resolve();keyfile=root/'key';keyfile.write_text(key)
 for mode in ('success','client-success','wrong-auth','wrong-boot','wrong-hash','disconnect','collision','no-finalize'):
  case=root/mode;case.mkdir();lock=case/'lock';tx=uuid.uuid4().hex
  if mode=='collision':(case/'STORE.OK').write_text('existing')
  with socket.socket() as reservation:reservation.bind(('127.0.0.1',0));port=reservation.getsockname()[1]
  env=dict(os.environ,BROOK_STORE_ROOT=str(case),BROOK_STORE_KEY=str(keyfile),BROOK_STORE_LOCK=str(lock),BROOK_STORE_PORT=str(port))
  process=subprocess.Popen([str(binary),boot,tx],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
  try:
   assert process.stdout.readline().startswith('HWSTORE_LISTENING')
   if mode=='client-success':
    result=provision('127.0.0.1',port,key,boot,tx,image,record,digest.hex())
    assert result['durable'] and result['transaction_id']==tx
   else:
    with socket.create_connection(('127.0.0.1',port),timeout=10) as conn:
     conn.settimeout(30);conn.sendall(b'BRSTORE1'+('0'*32 if mode=='wrong-auth' else key).encode()+('0'*32 if mode=='wrong-boot' else boot).encode()+tx.encode());state=reply(conn)
     if mode in ('wrong-auth','wrong-boot'):assert state['code']==10
     else:
      assert state['state']=='ready';conn.sendall(struct.pack('<I',len(record))+(bytes(32) if mode=='wrong-hash' else digest)+record+b'BEGIN001');state=reply(conn)
      if mode=='collision':assert state['code']==16
      else:
       assert state['state']=='receiving'
       if mode=='disconnect':conn.sendall(payload[:32768])
       else:
        conn.sendall(payload);state=reply(conn)
        if mode=='wrong-hash':assert state['code']==19
        else:
         assert state=={'state':'staged','published':False}
         if mode=='success':conn.sendall(b'FINALIZE');assert line(conn)=={'state':'completed','published':True,'durable':True}
   out,err=process.communicate(timeout=20);assert process.returncode==(0 if mode in ('success','client-success') else {'wrong-auth':10,'wrong-boot':10,'wrong-hash':19,'disconnect':18,'collision':16,'no-finalize':21}[mode]),(mode,out,err)
   if mode in ('success','client-success'):assert (case/'STORE.OK').read_bytes()==record and hashlib.sha256((case/'STORE.IMG').read_bytes()).digest()==digest and not lock.exists()
   elif mode=='collision':assert (case/'STORE.OK').read_text()=='existing' and not (case/'STORE.IMG').exists() and not lock.exists()
   elif mode in ('wrong-auth','wrong-boot'):assert not (case/'STORE.IMG').exists() and not lock.exists()
   else:assert (case/'STORE.IMG').exists() and not (case/'STORE.OK').exists() and lock.exists()
  finally:
   if process.poll() is None:process.kill();process.wait()
print('hardware store receiver loopback checks passed')
