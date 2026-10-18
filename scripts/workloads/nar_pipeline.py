#!/usr/bin/env python3
"""Owned NAR controls: generate synthetic archives, compare extraction stages.

No package/profile mutation. Each run writes fresh case directories under its
explicit fixture root, retains outputs, and verifies exact files and modes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import stat
import struct
import subprocess
import time


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream,'sha256').hexdigest()


def write_json(path,value):
    Path(path).write_text(json.dumps(value,indent=2)+'\n')


def generate(root,bulk_mib=32,tree_files=1024,zstd='zstd'):
    root=Path(root);root.mkdir(parents=True,exist_ok=False)
    if not 1<=bulk_mib<=128 or not 1<=tree_files<=4096:raise ValueError('Bounded fixture sizes required')
    manifest={'version':1,'bulk_mib':bulk_mib,'tree_files':tree_files,'shapes':{}}
    for shape in ['bulk','tree']:
        files={};archive=root/(shape+'.nar')
        with archive.open('wb') as stream:
            def string(value):
                data=value.encode() if isinstance(value,str) else value
                stream.write(struct.pack('<Q',len(data)));stream.write(data);stream.write(b'\0'*((-len(data))%8))
            def directory(children):
                string('(');string('type');string('directory')
                for name,child in children:
                    string('entry');string('(');string('name');string(name);string('node');child();string(')')
                string(')')
            def regular(name,blocks,size,executable=False):
                string('(');string('type');string('regular')
                if executable:string('executable');string('')
                string('contents');stream.write(struct.pack('<Q',size));sha=hashlib.sha256();written=0
                for data in blocks:
                    stream.write(data);sha.update(data);written+=len(data)
                assert written==size
                stream.write(b'\0'*((-size)%8));string(')')
                files[name]={'size':size,'sha256':sha.hexdigest(),'mode':0o755 if executable else 0o644}
            string('nix-archive-1')
            if shape=='bulk':
                def blocks():
                    for i in range(bulk_mib*16):
                        yield random.Random(1000+i).randbytes(32768)+b'\0'*32768
                directory([('payload.bin',lambda:regular('payload.bin',blocks(),bulk_mib*1024*1024))])
            else:
                groups=[]
                for group in range((tree_files+15)//16):
                    def emit_group(group=group):
                        children=[]
                        for index in range(group*16,min((group+1)*16,tree_files)):
                            name=f'd{group:04d}/f{index:04d}';data=(hashlib.sha256(name.encode()).digest()*33)[:1024+(index%2)]
                            children.append((f'f{index:04d}',lambda name=name,data=data,index=index:regular(name,[data],len(data),index%16==0)))
                        directory(children)
                    groups.append((f'd{group:04d}',emit_group))
                directory(groups)
        subprocess.run([zstd,'-q','-T1','-3',str(archive),'-o',str(archive)+'.zst'],check=True)
        manifest['shapes'][shape]={'files':files,'nar_sha256':digest(archive),'zst_sha256':digest(str(archive)+'.zst'),'nar_bytes':archive.stat().st_size,'zst_bytes':Path(str(archive)+'.zst').stat().st_size}
    write_json(root/'manifest.json',manifest)
    return manifest


def verify(destination,expected):
    found={};found_dirs=set()
    expected_dirs={str(p) for name in expected for p in Path(name).parents if str(p)!="."}
    for directory,dirs,names in os.walk(destination,followlinks=False):
        for name in dirs:
            path=Path(directory)/name
            if not stat.S_ISDIR(path.lstat().st_mode):raise ValueError('Unexpected directory link/type')
            found_dirs.add(str(path.relative_to(destination)))
        for name in names:
            path=Path(directory)/name;st=path.lstat()
            if not stat.S_ISREG(st.st_mode):raise ValueError('Unexpected output type')
            relative=str(path.relative_to(destination))
            found[relative]={'size':st.st_size,'sha256':digest(path),'mode':stat.S_IMODE(st.st_mode)}
    if found!=expected or found_dirs!=expected_dirs:raise ValueError('Extracted inventory/bytes/modes differ')
    return len(found)


def flush_output(destination):
    # Scope durability work to this owned extraction, including on the host.
    # Do not call global sync and flush unrelated host applications.
    directories=[]
    for directory,dirs,names in os.walk(destination,followlinks=False):
        directories.append(Path(directory))
        for name in names:
            fd=os.open(Path(directory)/name,os.O_RDONLY|os.O_NOFOLLOW)
            try:os.fsync(fd)
            finally:os.close(fd)
    for directory in reversed(directories):
        fd=os.open(directory,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
        try:os.fsync(fd)
        finally:os.close(fd)


def run(root,nar,cat,zstd,curl,url,rounds=2,transports=None):
    root=Path(root).absolute()
    if root.is_symlink() or root.resolve()!=root:raise ValueError('Fixture root must be a real absolute directory')
    manifest=json.loads((root/'manifest.json').read_text());output=root/'results';output.mkdir(exist_ok=False)
    os.umask(0o022)
    transports=transports or ['file','cat','zstd','http-zstd','http-raw']
    allowed={'file','cat','zstd','http-zstd','http-raw'}
    if not 1<=rounds<=3 or not transports or any(t not in allowed for t in transports):raise ValueError('Invalid bounded run selection')
    for shape,spec in manifest['shapes'].items():
        assert digest(root/(shape+'.nar'))==spec['nar_sha256']
        assert digest(root/(shape+'.nar.zst'))==spec['zst_sha256']
    rows=[]
    for round_number in range(rounds):
        order=transports if round_number%2==0 else list(reversed(transports))
        for shape,spec in manifest['shapes'].items():
            for transport in order:
                case=output/f'{shape}-{transport}-{round_number+1}';case.mkdir();destination=case/'unpacked';archive=root/(shape+'.nar')
                if transport=='file':commands=[[nar,str(destination)]]
                elif transport=='cat':commands=[[cat,str(archive)],[nar,str(destination)]]
                elif transport=='zstd':commands=[[zstd,'-dc',str(archive)+'.zst'],[nar,str(destination)]]
                elif transport=='http-zstd':commands=[[curl,'-fsS',url.rstrip('/')+'/'+shape+'.nar.zst'],[zstd,'-dc'],[nar,str(destination)]]
                else:commands=[[curl,'-fsS',url.rstrip('/')+'/'+shape+'.nar'],[nar,str(destination)]]
                before=Path('/proc/blkprobe').read_text() if Path('/proc/blkprobe').exists() else None
                children=[];logs=[];previous=None;source=None;started=time.monotonic_ns()
                try:
                    if transport=='file':source=archive.open('rb');previous=source
                    for index,argv in enumerate(commands):
                        log=(case/f'child-{index}.stderr').open('wb');logs.append(log)
                        child=subprocess.Popen(argv,stdin=previous if previous is not None else subprocess.DEVNULL,stdout=subprocess.PIPE if index+1<len(commands) else subprocess.DEVNULL,stderr=log)
                        children.append(child)
                        if previous is not None:previous.close()
                        previous=child.stdout
                    statuses=[p.wait() for p in reversed(children)][::-1]
                    extracted=time.monotonic_ns()
                    if any(statuses):raise RuntimeError('Pipeline failed: '+repr(statuses))
                    flush_output(destination);synced=time.monotonic_ns()
                finally:
                    if previous is not None:previous.close()
                    if source is not None:source.close()
                    for p in children:
                        if p.poll() is None:p.terminate()
                    for p in children:p.wait()
                    for log in logs:log.close()
                after=Path('/proc/blkprobe').read_text() if before is not None else None
                files=verify(destination,spec['files']);verified=time.monotonic_ns()
                row={'shape':shape,'transport':transport,'round':round_number+1,'bytes':sum(v['size'] for v in spec['files'].values()),'files':files,'extract_ns':extracted-started,'sync_ns':synced-extracted,'verify_ns':verified-synced,'statuses':statuses,'verified':True,'block_before':before,'block_after':after}
                rows.append(row);write_json(output/'report.json',{'uid':os.getuid(),'gid':os.getgid(),'rows':rows,'complete':False,'scope':'Synthetic data; cold/warm caches uncontrolled, second round reverses transport order. Extraction includes process startup; sync and verification timed separately. Whole-VM counters include observer work.'});print('NAR_CONTROL_RESULT '+json.dumps({k:v for k,v in row.items() if not k.startswith('block_')}),flush=True)
    report={'uid':os.getuid(),'gid':os.getgid(),'rows':rows,'complete':True,'scope':'Synthetic data; no package/profile mutation. Cold/warm caches uncontrolled, second round reverses transport order. Extraction includes process startup; sync and verification timed separately.'}
    write_json(output/'report.json',report);print('NAR_CONTROL_PASS',len(rows),flush=True)
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('action',choices=['generate','run','verify']);parser.add_argument('root',type=Path)
    parser.add_argument('--bulk-mib',type=int,default=32);parser.add_argument('--tree-files',type=int,default=1024);parser.add_argument('--rounds',type=int,default=2)
    parser.add_argument('--nar',default='nar-unpack');parser.add_argument('--cat',default='cat');parser.add_argument('--zstd',default='zstd');parser.add_argument('--curl',default='curl');parser.add_argument('--url',default='http://127.0.0.1:1');parser.add_argument('--transports',nargs='+')
    args=parser.parse_args()
    if args.action=='generate':generate(args.root,args.bulk_mib,args.tree_files,args.zstd)
    elif args.action=='run':run(args.root,args.nar,args.cat,args.zstd,args.curl,args.url,args.rounds,args.transports)
    else:
        manifest=json.loads((args.root/'manifest.json').read_text());report=json.loads((args.root/'results/report.json').read_text());assert report['complete']
        for row in report['rows']:verify(args.root/'results'/f"{row['shape']}-{row['transport']}-{row['round']}"/'unpacked',manifest['shapes'][row['shape']]['files'])
        print('NAR_CONTROL_REOPEN_PASS',len(report['rows']))
if __name__=='__main__':main()
