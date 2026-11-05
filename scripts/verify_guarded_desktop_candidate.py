#!/usr/bin/env python3
"""Verify private guarded desktop files; never open or modify a device.

The reference initrd and build metadata are evidence, not attestation or
current-boot identity. A successful result does not authorize installation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

from prepare_guard_check_pair import CHECKER_SHA, E2FSCK_SHA, REQUIRED, selector
from prepare_hardware_kernel_pair import INITRD_MAX, KERNEL_MAX, module_version, regular

FILES=('ETC/ahci-write-target','ETC/ahci-boot-volume','ETC/ahci-data-volume',
       'BOOT/FATFSCK.ELF','BOOT/E2FSCK.ELF')
EXPECTED=dict(REQUIRED, BROOK_SSD_AUTO_RECOVERY='ON', BROOK_SSD_GUARDED_DESKTOP='ON',
              BROOK_SSD_GUARDED_DESKTOP_RESET='ON',BROOK_MAPPING_TEST_HOOKS='OFF',
              BROOK_REGISTER_TEST_HOOKS='OFF',BROOK_SCHED_START_TEST_HOOKS='OFF',
              BROOK_EPOLL_TARGET_DIAGNOSTICS='OFF')
DERIVED={'BROOK_SSD_GUARD_TRANSACTION','BROOK_SSD_GUARD_REPAIR','BROOK_SSD_GUARD_COMPLETION'}

def sha(data):return hashlib.sha256(data).hexdigest()

def verify(candidate, source, reference, build, expected_kernel, expected_source, expected_reference, abi):
    if type(abi) is not int or abi<1:raise ValueError('invalid module ABI')
    image=regular(candidate/'INITRD.IMG',INITRD_MAX);kernel=regular(candidate/'kernel.elf',KERNEL_MAX)
    original=regular(source,INITRD_MAX);guard=regular(reference,INITRD_MAX)
    for label,actual,expected in (('kernel',sha(kernel),expected_kernel),('source',sha(original),expected_source),('reference',sha(guard),expected_reference)):
        if not re.fullmatch('[0-9a-f]{64}',expected) or actual!=expected:raise ValueError(label+' hash mismatch')
    if kernel!=regular(build/'BROOK.elf',KERNEL_MAX):raise ValueError('candidate kernel differs from build')
    cache=regular(build/'CMakeCache.txt',1024*1024).decode()
    for name,value in EXPECTED.items():
        if len(re.findall(r'^'+name+r':BOOL='+value+r'$',cache,re.M))!=1:raise ValueError('unsafe build option '+name)
    if len(re.findall(r'^BROOK_SMP_BOOT_TEST_STAGE:STRING=0$',cache,re.M))!=1:raise ValueError('unsafe SMP test stage')
    compile_data=regular(build/'compile_commands.json',16*1024*1024)
    commands=json.loads(compile_data)
    protected=set(EXPECTED)|DERIVED|{'BROOK_SMP_BOOT_TEST_STAGE'}
    expected_defs={k for k,v in EXPECTED.items() if v=='ON'}|DERIVED
    for name in ('kernel.cpp','ahci_readonly.cpp','ahci_system_volume.cpp','guard_checker_runner.cpp','shutdown.cpp'):
        matches=[item for item in commands if Path(item['file']).name==name]
        if len(matches)!=1:raise ValueError('missing compile evidence '+name)
        args=shlex.split(matches[0]['command']);defs=[];i=0
        while i<len(args):
            arg=args[i]
            if arg.startswith(('@','-include','-imacros','-Wp,')) or arg in ('-Xclang','-Xpreprocessor'):
                raise ValueError('unreviewable compile flags '+name)
            if arg in ('-D','-U'):
                i+=1
                if i>=len(args):raise ValueError('incomplete compile flags '+name)
                defs.append((arg,args[i]))
            elif arg.startswith(('-D','-U')) and len(arg)>2:defs.append((arg[:2],arg[2:]))
            i+=1
        found=[x for x in defs if x[1].split('=')[0] in protected]
        required_defs={('-D',x+'=1') for x in expected_defs}|{('-D','BROOK_SMP_BOOT_TEST_STAGE=0')}
        if len(found)!=len(required_defs) or set(found)!=required_defs:
            raise ValueError('unsafe compile flags '+name)
    with tempfile.TemporaryDirectory(prefix='brook-guarded-desktop-verify-') as tmp:
        work=Path(tmp);paths=[]
        for name,contents in (('candidate',image),('source',original),('reference',guard)):
            p=work/(name+'.img');p.write_bytes(contents);paths.append(p)
        live,base,ref=paths
        subprocess.run(['fsck.fat','-n',str(live)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        def read(path,name):return subprocess.check_output(['mtype','-i',str(path),'::/'+name],stderr=subprocess.DEVNULL)
        values={name:read(live,name) for name in FILES}
        for name in FILES:
            if values[name]!=read(base,name) or values[name]!=read(ref,name):raise ValueError('guard input mismatch '+name)
        if sha(values['BOOT/FATFSCK.ELF'])!=CHECKER_SHA or sha(values['BOOT/E2FSCK.ELF'])!=E2FSCK_SHA:
            raise ValueError('recovery checker pin mismatch')
        esp=selector(values['ETC/ahci-boot-volume'],'esp');data=selector(values['ETC/ahci-data-volume'],'data')
        if esp[0]!=data[0] or esp[1]==data[1] or not (esp[2]+esp[3]<=data[2] or data[2]+data[3]<=esp[2]):
            raise ValueError('inconsistent guard selectors')
        script=read(live,'BOOT/INIT.RC')
        if not script or script!=read(base,'BOOT/INIT.RC'):raise ValueError('desktop startup script mismatch')
        def modules(path):
            listing=subprocess.check_output(['mdir','-a','-b','-i',str(path),'::/DRIVERS/*'],text=True)
            names=[line.rsplit('/',1)[-1] for line in listing.splitlines() if line]
            if not names or len({n.lower() for n in names})!=len(names) or any(not re.fullmatch(r'[A-Za-z0-9_-]+\.[Mm][Oo][Dd]',n) for n in names):
                raise ValueError('invalid module inventory')
            return names
        names=modules(live)
        if {n.lower() for n in names}!={n.lower() for n in modules(base)}:raise ValueError('module inventory changed')
        module_hashes={}
        for name in names:
            contents=regular(build/'drivers'/name.lower(),16*1024*1024)
            if module_version(contents)!=abi or read(live,'DRIVERS/'+name)!=contents:raise ValueError('module mismatch '+name)
            module_hashes[name]=sha(contents)
    return {'scope':'private-file verification only; not deployment or physical boot authorization',
            'kernel_sha256':sha(kernel),'initrd_sha256':sha(image),'source_initrd_sha256':sha(original),
            'reference_initrd_sha256':sha(guard),'build_cache_sha256':sha(cache.encode()),
            'compile_commands_sha256':sha(compile_data),'startup_script_sha256':sha(script),
            'guard_input_sha256':{name:sha(value) for name,value in values.items()},'module_abi':abi,'modules':module_hashes}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('candidate','source','reference','build'):p.add_argument('--'+name,type=Path,required=True)
    for name in ('expected-kernel','expected-source','expected-reference'):p.add_argument('--'+name,required=True)
    p.add_argument('--abi',type=int,required=True)
    a=p.parse_args();print(json.dumps(verify(a.candidate,a.source,a.reference,a.build,a.expected_kernel,a.expected_source,a.expected_reference,a.abi),indent=2))

if __name__=='__main__':main()
