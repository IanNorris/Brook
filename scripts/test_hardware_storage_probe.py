#!/usr/bin/env python3
"""Test the scratch-only hardware write probe against private host files."""
import argparse, hashlib, json, os, struct, subprocess, uuid, zlib
from pathlib import Path

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--program',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args();program=args.program.resolve(strict=True);base=args.output.resolve()
base.mkdir(parents=True,exist_ok=False)
nonce=uuid.uuid4().hex
header=b'BROOK_IO_TEST_V1'+nonce.encode()+struct.pack('<Q',1048576)
header+=struct.pack('<II',zlib.crc32(header),0)
path=base/'scratch.bin';path.write_bytes(header+bytes(1048576-64))
def digest():return hashlib.sha256(path.read_bytes()).hexdigest()
def run(mode,file=path,key=nonce,expected=0):
    result=subprocess.run([str(program),mode,str(file),key],capture_output=True,text=True)
    assert result.returncode==expected,(mode,expected,result.returncode,result.stdout,result.stderr)
    return result.stdout
before=digest()
run('write',key='0'*32 if nonce!='0'*32 else '1'*32,expected=7);assert digest()==before
link=base/'symlink';link.symlink_to(path);run('write',file=link,expected=3);assert digest()==before
hard=base/'hardlink';os.link(path,hard);run('write',expected=5);assert digest()==before;hard.unlink()
run('verify',expected=9)
assert 'fresh_boot_verify_required=1' in run('write')
assert path.stat().st_size==1048576 and path.read_bytes()[:64]==header
assert 'HWIO_VERIFY_OK' in run('verify')
with path.open('r+b') as f:f.seek(65537);old=f.read(1);f.seek(65537);f.write(bytes([old[0]^1]))
run('verify',expected=9)
bad=base/'bad-header.bin';bad.write_bytes(bytes(1048576));run('write',file=bad,expected=7)
assert bad.read_bytes()==bytes(1048576)
short=base/'wrong-size.bin';short.write_bytes(header);run('write',file=short,expected=5);assert short.read_bytes()==header
(base/'result.json').write_text(json.dumps({'passed':True,'scope':'host private files; not physical durability','no_create_truncate_delete':True,'nonce_header_link_size_refusals':True,'corruption_detected':True},indent=2)+'\n')
print('HARDWARE_STORAGE_PROBE_HOST_PASS')
