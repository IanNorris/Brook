#!/usr/bin/env python3
"""Real binary receiver: fragmented traffic, failures, publication, readback."""
import argparse,hashlib,json,os,socket,struct,subprocess,sys,time,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
from hardware_bulk_protocol import authenticate,line,upload,download,frame_png
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--program',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);store=out/'stage';store.mkdir(mode=0o755)
key=uuid.uuid4().hex;boot=uuid.uuid4().hex;(out/'key').write_text(key);(out/'key').chmod(0o600)
data=bytes(range(251))*(2*1024*1024//251)+b'end';source=out/'source.bin';source.write_bytes(data);digest=hashlib.sha256(data).hexdigest()
results=[];saved=uuid.uuid4().hex
def case(name,action,callback,expected=0,transaction=None,artifact=None,published_failure=False):
    tx=transaction or uuid.uuid4().hex
    with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
    env={**os.environ,'BROOK_BULK_TEST_ROOT':str(store),'BROOK_BULK_TEST_KEY':str(out/'key'),'BROOK_BULK_TEST_PORT':str(port)}
    if published_failure:env['BROOK_BULK_TEST_PUBLISHED_FAILURE']='1'
    args=[str(a.program.resolve()),boot,tx,action]+([artifact] if artifact else [])
    log=out/(name+'.log')
    with log.open('w') as f:
        child=subprocess.Popen(args,env=env,stdout=f,stderr=subprocess.STDOUT,stdin=subprocess.DEVNULL)
        try:
            end=time.monotonic()+5
            while 'HWBULK_LISTENING' not in log.read_text():
                assert child.poll() is None,log.read_text();assert time.monotonic()<end;time.sleep(.01)
            callback(port,tx)
            assert child.wait(timeout=10)==expected,log.read_text()
        finally:
            if child.poll() is None:child.terminate();child.wait(timeout=5)
    assert not list(store.glob('*.part')),name+' retained partial artifact'
    results.append({'case':name,'exit':expected});return tx
def put(port,tx):
    r=upload('127.0.0.1',port,key,boot,tx,source);assert r['sha256']==digest and r['bytes']==len(data)
case('put-many-chunks','put',put,transaction=saved)
assert (store/(saved+'.bin')).read_bytes()==data
def get(port,tx):
    r=download('127.0.0.1',port,key,boot,tx,out/'download.bin');assert r['sha256']==digest
case('get-roundtrip','get',get,artifact=saved);assert (out/'download.bin').read_bytes()==data
def fragmented(port,tx):
    with socket.create_connection(('127.0.0.1',port)) as c:
        authenticate(c,key,boot,tx);c.sendall(struct.pack('<Q',len(data))+bytes.fromhex(digest))
        for offset in range(0,len(data),257):c.sendall(data[offset:offset+257])
        assert line(c)=={'state':'staged','published':False};c.sendall(b'FINALIZE');assert line(c)['state']=='completed'
case('fragmented-packets','put',fragmented)
def bad(port,tx,mode):
    with socket.create_connection(('127.0.0.1',port)) as c:
        if mode=='auth':
            c.sendall(b'BRBULK01'+b'0'*32+boot.encode()+tx.encode());assert json.loads(c.recv(1024))['code']==10;return
        if mode=='stale':
            c.sendall(b'BRBULK01'+key.encode()+b'0'*32+tx.encode());assert json.loads(c.recv(1024))['code']==10;return
        authenticate(c,key,boot,tx)
        if mode=='oversize':c.sendall(struct.pack('<Q',1024**3+1)+bytes(32));assert json.loads(c.recv(1024))['code']==13;return
        c.sendall(struct.pack('<Q',len(data))+bytes(32))
        if mode=='disconnect':c.sendall(data[:100]);return
        c.sendall(data);assert json.loads(c.recv(1024))['code']==16
for mode,code in [('auth',10),('stale',10),('oversize',13),('disconnect',15),('hash',16)]:
    case(mode,'put',lambda port,tx,m=mode:bad(port,tx,m),code)
def stage_only(port,tx):assert upload('127.0.0.1',port,key,boot,tx,source,False)['state']=='staged'
stage=case('no-finalize','put',stage_only,19);assert not (store/(stage+'.bin')).exists()
def exists(port,tx):
    with socket.create_connection(('127.0.0.1',port)) as c:
        authenticate(c,key,boot,tx);c.sendall(struct.pack('<Q',len(data))+bytes.fromhex(digest));assert json.loads(c.recv(1024))['code']==14
case('existing-artifact-preserved','put',exists,14,transaction=saved);assert (store/(saved+'.bin')).read_bytes()==data
symlink_id=uuid.uuid4().hex;symlink=store/(symlink_id+'.bin');symlink.symlink_to(out/'nonexistent')
case('dangling-symlink-preserved','put',exists,14,transaction=symlink_id);assert symlink.is_symlink()
def symlink_get(port,tx):
    try:download('127.0.0.1',port,key,boot,tx,out/'symlink-download.bin')
    except RuntimeError as error:assert error.args[0]['code']==22
    else:assert False
case('symlink-download-rejected','get',symlink_get,22,artifact=symlink_id);assert not (out/'symlink-download.bin').exists()
def published_failure(port,tx):
    try:upload('127.0.0.1',port,key,boot,tx,source)
    except RuntimeError as error:assert error.args[0]['published'] is True and error.args[0]['code']==21
    else:assert False
published=case('published-reply-failure','put',published_failure,21,published_failure=True)
assert (store/(published+'.bin')).read_bytes()==data
assert 'HWBULK_PUBLISHED ' in (out/'published-reply-failure.log').read_text()
host_existing=out/'host-existing.bin';host_existing.write_bytes(b'user data')
def host_collision(port,tx):
    try:download('127.0.0.1',port,key,boot,tx,host_existing)
    except FileExistsError:pass
    else:assert False
case('host-destination-preserved','get',host_collision,artifact=saved);assert host_existing.read_bytes()==b'user data'
# Verify channel order in the host PNG conversion with decoded IDAT bytes.
raw=out/'two-pixels.bgra';raw.write_bytes(bytes([0,0,255,255,255,0,0,255]))
png=out/'two-pixels.png';frame_png(raw,png,{'width':2,'height':1,'format':1})
import zlib
encoded=png.read_bytes();offset=8;compressed=b''
while offset<len(encoded):
    size=struct.unpack('>I',encoded[offset:offset+4])[0];kind=encoded[offset+4:offset+8];body=encoded[offset+8:offset+8+size]
    assert zlib.crc32(kind+body)&0xffffffff==struct.unpack('>I',encoded[offset+8+size:offset+12+size])[0]
    if kind==b'IDAT':compressed+=body
    offset+=12+size
assert zlib.decompress(compressed)==bytes([0,255,0,0,0,0,255])
results.append({'case':'BGRA-to-PNG-red-blue-pixels','passed':True})
(out/'result.json').write_text(json.dumps({'passed':True,'cases':results,'roundtrip_bytes':len(data),'sha256':digest},indent=2)+'\n')
print('HARDWARE_BULK_HOST_PASS',len(results))
