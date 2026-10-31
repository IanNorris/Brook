#!/usr/bin/env python3
"""Regression: a 30-second poll timeout is not the overall transfer deadline."""
import argparse,hashlib,os,socket,struct,subprocess,sys,tempfile,time,uuid
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from hardware_bulk_protocol import authenticate,line
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--program',required=True,type=Path);a=p.parse_args()
with tempfile.TemporaryDirectory() as tmp:
 root=Path(tmp);store=root/'store';store.mkdir();key=uuid.uuid4().hex;boot=uuid.uuid4().hex;tx=uuid.uuid4().hex
 keyfile=root/'key';keyfile.write_text(key);keyfile.chmod(0o600)
 with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
 env={**os.environ,'BROOK_BULK_TEST_ROOT':str(store),'BROOK_BULK_TEST_KEY':str(keyfile),'BROOK_BULK_TEST_PORT':str(port)}
 child=subprocess.Popen([str(a.program.resolve()),boot,tx,'put'],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 try:
  assert 'HWBULK_LISTENING' in child.stdout.readline()
  with socket.create_connection(('127.0.0.1',port),timeout=5) as c:
   c.settimeout(5);authenticate(c,key,boot,tx);time.sleep(31)
   data=b'after poll timeout';c.sendall(struct.pack('<Q',len(data))+hashlib.sha256(data).digest()+data)
   assert line(c)=={'state':'staged','published':False};c.sendall(b'FINALIZE');assert line(c)['state']=='completed'
  assert child.wait(timeout=5)==0;assert (store/(tx+'.bin')).read_bytes()==data
 finally:
  if child.poll() is None:child.terminate();child.wait(timeout=5)
print('HARDWARE_BULK_IDLE_DEADLINE_PASS')
