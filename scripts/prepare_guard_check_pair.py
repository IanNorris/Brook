#!/usr/bin/env python3
"""Prepare a private identity-bound SSD guard recovery pair.

This builds a fresh RAM initrd, never opens a disk, never installs anything,
and does not install or start recovery. Inventory inputs require independent review;
hashes and build metadata are evidence, not signed-kernel attestation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

from hardware_recovery_fsck import validate_checker
from prepare_ssd_recovery_pair import read_regular
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from hardware_update_protocol import record

CHECKER_SHA='99cc8160e7a076ecd9cb04e13bdbe71c4aad4c64bfdc43ea83a016c8fee9d5e3'
E2FSCK_SHA='acc6dee6fb21afd670d052617b665588070d536e2e73b5c4f95db11f98922268'
REQUIRED={'BROOK_SSD_GUARD_CHECK':'ON','BROOK_SSD_INSTALLER':'OFF','BROOK_AHCI_WRITE_TEST_HOOKS':'OFF','BROOK_AHCI_PANIC_TEST':'OFF','BROOK_AHCI_PANIC_CONTENTION_TEST':'OFF',
          'BROOK_SSD_GUARD_PENDING_TEST':'OFF','BROOK_SSD_GUARD_EXT2_TEST':'OFF','BROOK_SSD_GUARD_COMPLETE_TEST':'OFF',
          'BROOK_SSD_AUTO_RECOVERY':'OFF','BROOK_SSD_GUARDED_DESKTOP':'OFF',
          'BROOK_SSD_GUARDED_DESKTOP_RESET':'OFF',
          'BROOK_SSD_GUARDED_DESKTOP_RESET_TEST':'OFF'}


def sha(data):return hashlib.sha256(data).hexdigest()


def selector(data,kind):
    pattern=(rb'BRKESP1\n([0-9a-f]{32})\n([0-9a-f]{32})\n([1-9][0-9]*),([1-9][0-9]*)\n' if kind=='esp' else
             rb'BRKDATA1\n([0-9a-f]{32})\n([0-9a-f]{32})\n([1-9][0-9]*),([1-9][0-9]*)\n([0-9a-f]{32})\n')
    match=re.fullmatch(pattern,data)
    if not match:raise ValueError('invalid '+kind+' selector')
    disk,part,first,count=match.group(1,2,3,4)
    if int(disk,16)==0 or int(part,16)==0:raise ValueError('zero selector identity')
    extra=match.group(5) if kind=='data' else b''
    if kind=='data' and int(extra,16)==0:raise ValueError('zero filesystem UUID')
    first,count=int(first),int(count)
    if first>2**48 or count>2**48 or first+count>2**48:raise ValueError('selector geometry overflow')
    return disk,part,first,count,extra


def prepare(build,kernel_hash,checker,validator,controller,disk,esp,data,profile,out,automatic=False,e2fsck=None,recovery_network=False,module_abi=None,network_hash=None,compact_initrd=False,tiny_initrd=False):
    if profile not in ('physical','qemu-fixture'):raise ValueError('unsupported inventory profile')
    if compact_initrd and tiny_initrd:raise ValueError('choose one initrd size')
    if bool(e2fsck)!=automatic:raise ValueError('automatic recovery requires the pinned ext2 checker')
    if recovery_network and (not automatic or type(module_abi) is not int or module_abi<1 or
                             not isinstance(network_hash,str) or not re.fullmatch('[0-9a-f]{64}',network_hash)):
        raise ValueError('recovery networking requires automatic recovery and explicit module ABI and SHA-256')
    if not recovery_network and (module_abi is not None or network_hash is not None):raise ValueError('module options require recovery networking')
    sources={name:read_regular(path,1024*1024) for name,path in
             (('controller',controller),('disk',disk),('esp',esp),('data',data))}
    ctl=json.loads(sources['controller']);drive=json.loads(sources['disk'])
    if ctl.get('valid') is not True or type(ctl.get('controller_count')) is not int or ctl['controller_count']!=1 or drive.get('ready') is not True or drive.get('quarantined') is not False:
        raise ValueError('inventory not ready')
    numbers=[ctl.get(k) for k in ('vendor','device','bus','slot','function')]+[drive.get(k) for k in ('port','sectors','logical_sector_bytes','physical_sector_bytes')]
    if any(type(x) is not int for x in numbers):raise ValueError('invalid numeric inventory')
    vendor,device,bus,slot,function,port,sectors,logical,physical=numbers
    if not (0<=vendor<=65535 and 0<=device<=65535 and 0<=bus<=255 and 0<=slot<=31 and 0<=function<=7 and 0<=port<=31 and
            68<=sectors<=2**48 and logical==512 and 512<=physical<=16777216 and physical&(physical-1)==0):
        raise ValueError('unsupported hardware geometry')
    if sum(p.get('index')==port and p.get('signature')==257 for p in ctl.get('ports',[]) if isinstance(p,dict))!=1:
        raise ValueError('controller port observation mismatch')
    model,serial=drive.get('model'),drive.get('serial')
    def text_ok(v,n,leading):return isinstance(v,str) and 0<len(v)<=n and (leading or not v.startswith(' ')) and not v.endswith(' ') and all(32<=ord(c)<=126 and c!='?' for c in v)
    if not text_ok(model,40,False) or not text_ok(serial,20,True):raise ValueError('invalid ATA identity')
    if profile=='physical' and ('QEMU' in model.upper() or serial.startswith('BROOK-')):raise ValueError('fixture identity in physical profile')
    if profile=='qemu-fixture' and ((vendor,device,bus,slot,function,port,sectors,logical,physical)!=(32902,10530,0,31,2,0,262144,512,512) or model!='QEMU HARDDISK' or serial!='BROOK-RECOVERY-QA'):
        raise ValueError('fixture profile identity mismatch')
    eb,ep,ef,ec,_=selector(sources['esp'],'esp');db,dp,df,dc,fs=selector(sources['data'],'data')
    if eb!=db or ep==dp or min(ef,df)<34 or max(ef+ec,df+dc)>sectors-33 or not (ef+ec<=df or df+dc<=ef) or dc<4:
        raise ValueError('inconsistent disk selectors')
    target=('BRKAHCI1\n'+','.join(map(str,numbers))+'\n'+model+'\n'+serial+'\n').encode('ascii')
    if len(target)>192:raise ValueError('target record too long')
    cache=read_regular(build/'CMakeCache.txt',1024*1024).decode('utf-8')
    required=dict(REQUIRED)
    if automatic:required['BROOK_SSD_AUTO_RECOVERY']='ON'
    for name,wanted in required.items():
        if len(re.findall(r'^'+name+r':BOOL='+wanted+r'$',cache,re.M))!=1:raise ValueError('unsafe kernel build option '+name)
    compile_data=read_regular(build/'compile_commands.json',16*1024*1024)
    commands=json.loads(compile_data)
    for name in ('kernel.cpp','ahci_readonly.cpp','ahci_system_volume.cpp','guard_checker_runner.cpp'):
        matches=[x for x in commands if Path(x['file']).name==name]
        if len(matches)!=1:raise ValueError('missing compile evidence for '+name)
        args=shlex.split(matches[0]['command']);defines=[];i=0
        while i<len(args):
            arg=args[i]
            if arg.startswith(('@','-include','-imacros','-Wp,')) or arg in ('-Xclang','-Xpreprocessor'):
                raise ValueError('unreviewable compile flags in '+name)
            if arg in ('-D','-U'):
                i+=1
                if i>=len(args):raise ValueError('incomplete compile flags in '+name)
                defines.append((arg,args[i]))
            elif arg.startswith(('-D','-U')) and len(arg)>2:defines.append((arg[:2],arg[2:]))
            i+=1
        expected={'BROOK_SSD_GUARD_CHECK'}
        if automatic:expected.update(('BROOK_SSD_AUTO_RECOVERY','BROOK_SSD_GUARD_TRANSACTION',
                                      'BROOK_SSD_GUARD_REPAIR','BROOK_SSD_GUARD_COMPLETION'))
        protected=set(REQUIRED)|{'BROOK_SSD_GUARD_TRANSACTION','BROOK_SSD_GUARD_REPAIR','BROOK_SSD_GUARD_COMPLETION'}
        observed=[(op,value) for op,value in defines if value.split('=')[0] in protected]
        if len(observed)!=len(expected) or set(observed)!={('-D',key+'=1') for key in expected}:
            raise ValueError('unsafe compile flags in '+name)
    kernel=read_regular(build/'BROOK.elf',8*1024*1024)
    if not re.fullmatch('[0-9a-f]{64}',kernel_hash) or sha(kernel)!=kernel_hash:raise ValueError('kernel digest mismatch')
    subprocess.run([str(validator)],input=kernel,check=True)
    checker_info=validate_checker(checker)
    if checker_info['sha256']!=CHECKER_SHA:raise ValueError('checker does not match kernel pin')
    checker_data=read_regular(checker,4*1024*1024)
    if sha(checker_data)!=checker_info['sha256']:raise ValueError('checker changed during read')
    ext2_info=None;ext2_data=None
    if automatic:
        ext2_info=validate_checker(e2fsck)
        if ext2_info['sha256']!=E2FSCK_SHA:raise ValueError('ext2 checker does not match kernel pin')
        ext2_data=read_regular(e2fsck,4*1024*1024)
        if sha(ext2_data)!=ext2_info['sha256']:raise ValueError('ext2 checker changed during read')
    network_data=None;network_name=None
    if recovery_network:
        from prepare_hardware_kernel_pair import module_version
        network_name='xhci.mod' if profile=='physical' else 'virtio_net.mod'
        network_data=read_regular(build/'drivers'/network_name,4*1024*1024)
        if sha(network_data)!=network_hash:raise ValueError('recovery network module hash mismatch')
        if module_version(network_data)!=module_abi:raise ValueError('recovery network module ABI mismatch')
    out.mkdir(mode=0o700,parents=True,exist_ok=False)
    try:
        image=out/'INITRD.IMG';(out/'KERNEL.ELF').write_bytes(kernel)
        with image.open('wb') as f:f.truncate((8 if tiny_initrd else 16 if compact_initrd else 32)*1024*1024)
        subprocess.run(['mformat','-i',str(image),'::'],check=True)
        for name in ('BOOT','ETC','DEV','DEV/DRI','TMP','SYS','SYS/DEV','SYS/DEV/CHAR','USR','PROC'):
            subprocess.run(['mmd','-i',str(image),'::'+name],check=True)
        if recovery_network:subprocess.run(['mmd','-i',str(image),'::DRIVERS'],check=True)
        payload={'ETC/ahci-observe':b'1\n','ETC/ahci-read-only':b'1\n','ETC/ahci-write-target':target,
                 'ETC/ahci-boot-volume':sources['esp'],'ETC/ahci-data-volume':sources['data'],
                 'BOOT/FATFSCK.ELF':checker_data}
        if automatic:payload['BOOT/E2FSCK.ELF']=ext2_data
        if recovery_network:payload['DRIVERS/'+network_name]=network_data
        for name,content in payload.items():
            subprocess.run(['mcopy','-i',str(image),'-','::'+name],input=content,check=True)
            if subprocess.check_output(['mtype','-i',str(image),'::'+name])!=content:raise RuntimeError('initrd readback failed: '+name)
        initrd=image.read_bytes();selection=record(1,0,kernel,initrd);(out/'SELECT.BIN').write_bytes(selection)
        receipt={'scope':('automatic recovery pair' if automatic else 'read-only recovery pair')+' prepared privately; not installed or boot tested',
                 'profile':profile,'input_sha256':{name:sha(value) for name,value in sources.items()},
                 'build_cache_sha256':sha(cache.encode()),'compile_commands_sha256':sha(compile_data),
                 'kernel_build_flags':required,'checker_sha256':checker_info['sha256'],
                 'guard_identity':(db+dp+fs).decode(),'data_first':df,'data_sectors':dc,
                 'files':{name:{'bytes':len(value),'sha256':sha(value)} for name,value in
                          (('KERNEL.ELF',kernel),('INITRD.IMG',initrd),('SELECT.BIN',selection))}}
        if automatic:receipt['ext2_checker_sha256']=ext2_info['sha256']
        if recovery_network:receipt['recovery_network_module']={'name':network_name,'abi':module_abi,'sha256':sha(network_data)}
        (out/'manifest.json').write_text(json.dumps(receipt,indent=2)+'\n')
        return receipt
    except BaseException:
        shutil.rmtree(out);raise


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('kernel-build','checker','elf-validator','controller','disk','esp-selector','data-selector','output'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--kernel-sha256',required=True);p.add_argument('--profile',choices=('physical','qemu-fixture'),required=True)
    p.add_argument('--automatic',action='store_true',help='Prepare the isolated automatic recovery configuration')
    p.add_argument('--e2fsck',type=Path,help='Pinned static ext2 checker; required for automatic recovery')
    p.add_argument('--recovery-network',action='store_true',help='Include this build\'s network driver for read-only recovery debug')
    p.add_argument('--module-abi',type=int,help='Required expected network module ABI')
    p.add_argument('--network-module-sha256',help='Required expected SHA-256 of the selected build network module')
    p.add_argument('--compact-initrd',action='store_true',help='Use a 16 MiB FAT initrd for limited ESP space')
    p.add_argument('--tiny-initrd',action='store_true',help='Use an 8 MiB FAT initrd for limited ESP space')
    a=p.parse_args();print(json.dumps(prepare(a.kernel_build,a.kernel_sha256,a.checker,a.elf_validator,a.controller,a.disk,
                                               a.esp_selector,a.data_selector,a.profile,a.output,a.automatic,a.e2fsck,a.recovery_network,a.module_abi,a.network_module_sha256,a.compact_initrd,a.tiny_initrd),indent=2))


if __name__=='__main__':main()
