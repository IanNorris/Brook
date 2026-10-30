#!/usr/bin/env python3
"""Exercise the real maintenance receiver against private preallocated files."""
import argparse,hashlib,json,os,socket,subprocess,sys,time,uuid
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from hardware_update_protocol import record,authenticate,line,upload,KERNEL_MAX,INITRD_MAX
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--program',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
key=uuid.uuid4().hex;boot=uuid.uuid4().hex;tx=uuid.uuid4().hex
(out/'key').write_text(key)
import struct
kernel=bytearray(4096);kernel[:7]=b'\x7fELF\x02\x01\x01'
struct.pack_into('<HHIQQ',kernel,16,2,62,1,0xffffffff80000000,64)
struct.pack_into('<HHH',kernel,52,64,56,1)
struct.pack_into('<IIQQQQQ',kernel,64,1,1,0,0xffffffff80000000,0,len(kernel),len(kernel))
kernel=bytes(kernel);initrd=bytes(510)+b'\x55\xaa'
for slot in 'AB':
    (out/slot).mkdir()
    for name,size,data in [('KERNEL.ELF',KERNEL_MAX,kernel),('INITRD.IMG',INITRD_MAX,initrd)]:
        with (out/slot/name).open('wb') as f:f.write(data);f.truncate(size)
(out/'SELECT0.BIN').write_bytes(record(1,0,kernel,initrd));(out/'SELECT1.BIN').write_bytes(bytes(128))
files=list((out/'A').iterdir())+list((out/'B').iterdir())+[out/'SELECT0.BIN',out/'SELECT1.BIN']
def hashes():
    result={}
    for p in files:
        with p.open('rb') as stream:result[str(p.relative_to(out))]=hashlib.file_digest(stream,'sha256').hexdigest()
    return result
def run_case(name,action,expected,unchanged=True,failure=None):
    before=hashes()
    with socket.socket() as r:r.bind(('127.0.0.1',0));port=r.getsockname()[1]
    env={**os.environ,'BROOK_UPDATE_TEST_ROOT':str(out),'BROOK_UPDATE_TEST_KEY':str(out/'key'),
        'BROOK_UPDATE_TEST_LOCK':str(out/'lock'),'BROOK_UPDATE_TEST_PORT':str(port)}
    if failure:env['BROOK_UPDATE_TEST_FAILURE']=failure
    with (out/(name+'.log')).open('w') as log:
        proc=subprocess.Popen([str(a.program.resolve()),boot,tx],env=env,stdout=log,stderr=subprocess.STDOUT)
        try:
            deadline=time.monotonic()+5
            while 'HWUPDATE_LISTENING' not in (out/(name+'.log')).read_text():
                assert proc.poll() is None,(name,proc.returncode)
                assert time.monotonic()<deadline;time.sleep(.01)
            action(port)
            assert proc.wait(timeout=10)==expected,(name,proc.returncode)
        finally:
            if proc.poll() is None:proc.terminate();proc.wait(timeout=5)
    if unchanged:assert hashes()==before,name+' modified files'
    return {'case':name,'exit':expected,'unchanged':unchanged}
def auth_bad(port):
    with socket.create_connection(('127.0.0.1',port)) as c:
        c.sendall(b'BRUPLOAD'+b'0'*32+boot.encode()+tx.encode());assert line(c)['code']==10
def stale(port):
    with socket.create_connection(('127.0.0.1',port)) as c:
        c.sendall(b'BRUPLOAD'+key.encode()+b'0'*32+tx.encode());assert line(c)['code']==10
def transfer(port,mode):
    with socket.create_connection(('127.0.0.1',port)) as c:
        plan=authenticate(c,key,boot,tx);raw=record(plan['generation'],plan['slot'],kernel,initrd)
        if mode=='oversize':
            import struct,zlib
            raw=bytearray(raw);struct.pack_into('<Q',raw,24,KERNEL_MAX+1);struct.pack_into('<I',raw,124,zlib.crc32(raw[:124]));c.sendall(raw);assert line(c)['code']==16;return
        c.sendall(raw)
        if mode=='interrupted':c.sendall(kernel[:30]);return
        c.sendall(kernel+(bytes(512) if mode=='corrupt' else initrd))
        if mode=='corrupt':assert line(c)['code']==19;return
        assert line(c)['state']=='staged'
        if mode=='staged':return
        c.sendall(b'ACTIVATE');assert line(c)['state']=='activated'
results=[run_case('unauthorized',auth_bad,10),run_case('stale-boot',stale,10)]
for mode,expected in [('oversize',16),('interrupted',18),('corrupt',19),('staged',22)]:
    results.append(run_case(mode,lambda port,m=mode:transfer(port,m),expected))
results.append(run_case('activate',lambda port:transfer(port,'activate'),0,False))
assert (out/'SELECT1.BIN').read_bytes()==record(2,1,kernel,initrd)
def operational_failure(port):
    with socket.create_connection(('127.0.0.1',port)) as c:
        c.sendall(b'BRUPLOAD'+key.encode()+boot.encode()+tx.encode());assert line(c)['code']==27
for failure in ['alloc','read']:
    results.append(run_case('newest-'+failure+'-failure',operational_failure,27,failure=failure))
# Valid metadata with a corrupt newest payload must preserve the older working
# slot, choose the other slot, and advance beyond the damaged generation.
with (out/'B/KERNEL.ELF').open('r+b') as f:f.write(b'bad')
results.append(run_case('fallback-active',lambda port:transfer(port,'activate'),0,False))
assert (out/'SELECT1.BIN').read_bytes()==record(3,1,kernel,initrd)
# A correctly hashed but structurally invalid newer ELF is not the active slot.
bad=bytearray(kernel);struct.pack_into('<Q',bad,32,2**64-1);bad=bytes(bad)
with (out/'B/KERNEL.ELF').open('r+b') as f:f.write(bad)
(out/'SELECT1.BIN').write_bytes(record(4,1,bad,initrd))
a_before=(out/'A/KERNEL.ELF').read_bytes()
results.append(run_case('malformed-newest-elf',lambda port:transfer(port,'activate'),0,False))
assert (out/'SELECT1.BIN').read_bytes()==record(5,1,kernel,initrd)
assert (out/'A/KERNEL.ELF').read_bytes()==a_before
# Equal generations referring to different pairs fail closed before staging.
(out/'SELECT0.BIN').write_bytes(record(5,0,kernel,initrd))
def conflict(port):
    with socket.create_connection(('127.0.0.1',port)) as c:
        c.sendall(b'BRUPLOAD'+key.encode()+boot.encode()+tx.encode());assert line(c)['code']==12
results.append(run_case('conflicting-records',conflict,12))
(out/'result.json').write_text(json.dumps({'passed':True,'cases':results},indent=2)+'\n')
print('HARDWARE_UPDATE_HOST_PASS')
