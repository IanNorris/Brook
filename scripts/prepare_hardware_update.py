#!/usr/bin/env python3
"""Prepare a RAM maintenance A/B profile, preserving root recovery files.

This opens private files only. It neither mounts nor modifies physical devices.
Run after preparing and testing the writable scratch profile. A/B slots update
kernel + initrd disk image; APPS.BRK and the EFI executable are not remotely
writable by the helper. Normal desktop recovery remains intrinsically RO.
"""
import argparse,hashlib,json,os,shutil,subprocess,sys,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
from hardware_update_protocol import record,KERNEL_MAX,INITRD_MAX
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--recovery-probe',type=Path,required=True);p.add_argument('--scratch-probe',type=Path,required=True)
p.add_argument('--output',type=Path,required=True);p.add_argument('--build',type=Path,default=ROOT/'build/release')
a=p.parse_args();recovery=a.recovery_probe.resolve(strict=True);scratch=a.scratch_probe.resolve(strict=True);out=a.output.resolve();build=a.build.resolve(strict=True)
if not os.environ.get('MUSL_CC'):p.error('Run inside nix-shell with MUSL_CC')
def run(*cmd):subprocess.run(list(map(str,cmd)),check=True,cwd=ROOT,stdin=subprocess.DEVNULL)
def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
meta=json.loads((recovery/'manifest.json').read_text());sm=json.loads((scratch/'manifest.json').read_text())
if meta['usb_read_only'] is not True or sm['usb_read_only'] is not False:p.error('Expected RO recovery and RW scratch profiles')
for probe,m in [(recovery,meta),(scratch,sm)]:
    for name,row in m['files'].items():
        target=probe/'payload'/name
        if target.is_symlink() or sha(target)!=row['sha256'] or target.stat().st_size!=row['bytes']:p.error('Input mismatch: '+str(target))
if (meta['files']['KERNEL/BROOK.ELF']['sha256']!=sm['files']['KERNEL/BROOK.ELF']['sha256'] or
    sha(build/'kernel/BROOK.elf')!=meta['files']['KERNEL/BROOK.ELF']['sha256']):
    p.error('Recovery kernel and maintenance module profile must match')
out.mkdir(parents=True,exist_ok=False)
run('cp','--reflink=auto','-a',recovery/'payload',out/'payload')
run('cp','--reflink=auto',recovery/'probe.raw',out/'probe.raw')
# Only the one-time bootstrap replaces EFI/config. Root recovery kernel/initrd,
# apps and control data remain byte-identical to physical read acceptance.
shutil.copy2(build/'bootloader/BOOTX64.efi',out/'payload/EFI/BOOT/BOOTX64.EFI')
config=out/'payload/BROOK.CFG';config.write_text(config.read_text()+'UPDATE_SLOTS=1\n')
updates=out/'payload/UPDATES';updates.mkdir()
for name in 'AB':(updates/name).mkdir()
maintenance=out/'maintenance.img';run('cp','--reflink=auto',scratch/'payload/INITRD.IMG',maintenance)
run('mcopy','-o','-i',maintenance,build/'kernel/drivers/xhci.mod','::DRIVERS/XHCI.MOD')
for source,dest in [('tools/hardware_update.c','HWUPDATE.ELF'),('tools/hardware_status.c','HWSTATUS.ELF')]:
    binary=out/dest;run(os.environ['MUSL_CC'],'-static','-O2','-Wall','-Wextra','-Werror',ROOT/source,'-o',binary)
    run('mcopy','-o','-i',maintenance,binary,'::BOOT/'+dest)
key=out/'hardware-update-key';key.write_text(uuid.uuid4().hex);key.chmod(0o600)
marker=out/'hardware-maintenance';marker.write_text('Opt-in RAM maintenance; no USB application image mount\n')
payload_id='maintenance-bootstrap-'+uuid.uuid4().hex
identity=out/'hardware-payload-id';identity.write_text(payload_id+'\n')
for file in [key,marker,identity]:run('mcopy','-o','-i',maintenance,file,'::ETC/'+file.name)
kernel=(out/'payload/KERNEL/BROOK.ELF').read_bytes();initrd=maintenance.read_bytes()
selection=record(1,0,kernel,initrd)
for slot in 'AB':
    for name,capacity,data in [('KERNEL.ELF',KERNEL_MAX,kernel if slot=='A' else b''),('INITRD.IMG',INITRD_MAX,initrd if slot=='A' else b'')]:
        with (updates/slot/name).open('wb') as f:f.write(data);f.truncate(capacity)
(updates/'SELECT0.BIN').write_bytes(selection);(updates/'SELECT1.BIN').write_bytes(bytes(128))
# Include the already-tested nonce gate so hardware writes can be checked before
# the first real activation, without yet touching a boot slot.
(out/'payload/BROOK-TEST').mkdir()
shutil.copy2(scratch/'payload/BROOK-TEST/IO-CONTROL.BIN',out/'payload/BROOK-TEST/IO-CONTROL.BIN')
media=str(out/'probe.raw')+'@@1048576'
for directory in ['UPDATES','UPDATES/A','UPDATES/B','BROOK-TEST']:run('mmd','-i',media,'::'+directory)
files={}
for path in sorted((out/'payload').rglob('*')):
    if not path.is_file():continue
    name=path.relative_to(out/'payload').as_posix();run('mcopy','-o','-i',media,path,'::'+name)
    files[name]={'bytes':path.stat().st_size,'sha256':sha(path)}
mutable=[name for name in files if name.startswith(('UPDATES/','BROOK-TEST/'))]
meta.update(files=files,test_disk=str(out/'probe.raw'),usb_read_only=False,scope='RAM maintenance A/B bootstrap; root recovery pair unchanged',
    mutable_files=mutable,scratch_nonce=sm['scratch_nonce'],scratch_path=sm['scratch_path'],maintenance_payload_id=payload_id,
    update_key_file=str(key),physical_status='Prepared only; QEMU and physical write/flush gates required',
    remote_update_scope=['kernel ELF','initrd FAT disk image'],remote_update_excludes=['EFI executable','partition table','APPS.BRK'])
(out/'manifest.json').write_text(json.dumps(meta,indent=2)+'\n')
print('HARDWARE_UPDATE_PROFILE_PREPARED',out)
