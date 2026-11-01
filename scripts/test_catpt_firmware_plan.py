#!/usr/bin/env python3
"""Compare the freestanding CATPT plan against the offline Python inventory."""
import argparse,random,struct,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from catpt_firmware import inspect

def fixture(blocks):
    body=b''.join(struct.pack('<4I',kind,len(data),offset,0)+data for kind,offset,data in blocks)
    module=struct.pack('<4sIIHHIII',b'$SST',len(body),len(blocks),0,0,0,0,0)+body
    return struct.pack('<4s7I',b'$SST',len(module),1,254,0,0,0,0)+module

def multi_fixture():
    def module(mid,slot,entry,persistent,scratch,blocks):
        body=b''.join(struct.pack('<4I',kind,len(data),offset,0)+data for kind,offset,data in blocks)
        return struct.pack('<4sIIHHIII',b'$SST',len(body),len(blocks),slot,mid,entry,persistent,scratch)+body
    contents=(module(3,7,0x1234,0x70,0x40,[(1,0,b'abcd')])+
              module(4,8,0x2345,0x80,0x50,[])+
              module(5,9,0x3456,0x90,0x60,[(2,8,b'efgh')]))
    return struct.pack('<4s7I',b'$SST',len(contents),3,254,0,0,0,0)+contents

def main():
    p=argparse.ArgumentParser();p.add_argument('--runner',type=Path,required=True);p.add_argument('--firmware',type=Path)
    a=p.parse_args();runner=a.runner.resolve(strict=True)
    samples=[fixture([(1,0,b'abcd')]),fixture([(1,0x4fffc,b'abcd'),(2,0x9fffc,b'efgh')]),
             fixture([(2,0,b'abcd'),(3,4,b'efgh')]),fixture([]),multi_fixture(),
             fixture([(2,i,b'x') for i in range(256)])]
    if a.firmware:samples.append(a.firmware.read_bytes())
    bad=[];base=samples[0]
    bad.extend(base[:n] for n in (0,1,31,32,59,60,75,79))
    for off in (4,8,12,36,40,60,64,68):
        b=bytearray(base);struct.pack_into('<I',b,off,0xffffffff);bad.append(bytes(b))
    bad.extend([fixture([(1,0,b'abcd'),(1,2,b'efgh')]),fixture([(1,0x4fffd,b'abcd')]),
                fixture([(2,0,b'abcd'),(3,0,b'efgh')]),fixture([(4,0,b'abcd')])])
    module=base[32:];duplicate=module+module
    bad.append(struct.pack('<4s7I',b'$SST',len(duplicate),2,254,0,0,0,0)+duplicate)
    # The last block is invalid after a valid first block; no partial plan is
    # published and the runner performs no mock copies on rejection.
    bad.append(fixture([(1,0,b'abcd'),(2,0x9fffd,b'efgh')]))
    # Python intentionally has no block-count cap; the freestanding planner
    # rejects this valid but unsupported image before publishing any plan.
    capped=fixture([(2,i,b'x') for i in range(257)])
    if a.firmware:
        rng=random.Random(9343);original=samples[-1]
        for _ in range(128):
            mutated=bytearray(original);at=rng.randrange(len(mutated));mutated[at]^=1<<rng.randrange(8)
            samples.append(bytes(mutated))
    accepted=rejected=0
    with tempfile.TemporaryDirectory() as temp:
        path=Path(temp)/'image'
        assert len(inspect(capped)['modules'][0]['blocks'])==257
        path.write_bytes(capped)
        limited=subprocess.run([str(runner),str(path)],capture_output=True,text=True)
        assert limited.returncode==1 and limited.stdout.strip()=='invalid'
        for data in samples+bad:
            path.write_bytes(data);r=subprocess.run([str(runner),str(path)],capture_output=True,text=True)
            try: info=inspect(data)
            except ValueError: info=None
            assert (r.returncode==0)==(info is not None),(len(data),r.stdout,r.stderr)
            if info:
                accepted+=1
                blocks=[(dict(iram=1,dram=2,instance=3)[b['type']],b['payload_offset'],b['size'],b['ram_offset']) for m in info['modules'] for b in m['blocks']]
                modules=[];first=0
                for m in info['modules']:
                    modules.append((m['id'],m['slot'],m['entry'],m['persistent'],m['scratch'],first,len(m['blocks'])));first+=len(m['blocks'])
                observedModules=[];observedBlocks=[]
                for line in r.stdout.splitlines():
                    tag,*values=line.split();assert tag in ('M','B')
                    (observedModules if tag=='M' else observedBlocks).append(tuple(map(int,values)))
                assert observedModules==modules and observedBlocks==blocks,(observedModules,modules,observedBlocks,blocks)
                assert f"modules={info['module_count']} blocks={len(blocks)}" in r.stderr
            else:
                rejected+=1
                assert r.returncode==1 and r.stdout.strip()=='invalid'
    print(f'CATPT_PLAN_DIFFERENTIAL_PASS accepted={accepted} rejected={rejected}')
if __name__=='__main__':main()
