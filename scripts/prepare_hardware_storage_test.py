#!/usr/bin/env python3
"""Prepare a disposable writable USB scratch-test profile; never open host devices.

Uses an accepted offline desktop probe as its recoverable base. Applications
remain on the image but are not mounted. Runtime, diagnostics and probe are RAM
resident; writes require an explicit HWIO invocation against a provisioned file.
Run in nix-shell with MUSL_CC and the current xHCI module built.
"""
import argparse, hashlib, json, os, shutil, struct, subprocess, uuid, zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--base-probe',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--build',type=Path,default=ROOT/'build/release')
args=parser.parse_args();base=args.base_probe.resolve(strict=True);out=args.output.resolve();build=args.build.resolve(strict=True)
if not os.environ.get('MUSL_CC'):parser.error('Run inside nix-shell with MUSL_CC')
for p in [base/'manifest.json',base/'probe.raw',build/'kernel/BROOK.elf',build/'kernel/drivers/xhci.mod']:
    if not p.is_file():parser.error('Missing '+str(p))
meta=json.loads((base/'manifest.json').read_text())
if meta.get('usb_read_only') is not True:parser.error('Base must be the accepted read-only profile')
payload=base/'payload'
if payload.is_symlink():parser.error('Symlink payload root is not supported')
for p in payload.rglob('*'):
    if p.is_symlink() or (p.is_file() and p.relative_to(payload).as_posix() not in meta['files']):
        parser.error('Unexpected/symlink base entry: '+str(p))
def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
for name,row in meta['files'].items():
    p=base/'payload'/name
    if sha(p)!=row['sha256'] or p.stat().st_size!=row['bytes']:parser.error('Base payload hash mismatch: '+name)
out.mkdir(parents=True,exist_ok=False)
def run(*argv):return subprocess.run(list(map(str,argv)),check=True,cwd=ROOT)
run('cp','--reflink=auto','-a',base/'payload',out/'payload')
run('cp','--reflink=auto',base/'probe.raw',out/'probe.raw')
shutil.copy2(build/'kernel/BROOK.elf',out/'payload/KERNEL/BROOK.ELF')
program=out/'HWIO.ELF'
run(os.environ['MUSL_CC'],'-static','-O2','-Wall','-Wextra','-Werror',ROOT/'tools/hardware_storage_probe.c','-o',program)
initrd=out/'payload/INITRD.IMG'
# A writable provider is established at initialization. Never change a running
# device's permanent read-only guarantee beneath a retained image source.
for name in ['usb-read-only','boot-app-image']:
    run('mdel','-i',initrd,'::ETC/'+name)
run('mcopy','-o','-i',initrd,build/'kernel/drivers/xhci.mod','::DRIVERS/XHCI.MOD')
run('mcopy','-i',initrd,program,'::BOOT/HWIO.ELF')
marker=out/'hardware-storage-test';marker.write_text('writable scratch test; no application image mount\n')
run('mcopy','-i',initrd,marker,'::ETC/hardware-storage-test')
rc=out/'INIT.RC';rc.write_text('set wm\nset vfb none\nsched eevdf\nkconsole\nrun --console --uid 0 /boot/HWSTATUS.ELF\n')
run('mcopy','-o','-i',initrd,rc,'::BOOT/INIT.RC')
nonce=uuid.uuid4().hex;test_name='BROOK-TEST/IO-CONTROL.BIN';test=out/'payload'/test_name;test.parent.mkdir()
header=b'BROOK_IO_TEST_V1'+nonce.encode()+struct.pack('<Q',1048576)
header+=struct.pack('<II',zlib.crc32(header),0)
test.write_bytes(header+bytes(1048576-64))
readme=out/'payload/README.TXT'
readme.write_text('Brook writable hardware scratch-test profile\n\n'
    'USB storage is WRITABLE in this explicit test profile. Applications are not mounted.\n'
    'Diagnostics and runtime reside in RAM. No automatic write test runs at boot.\n'
    'Only invoke /boot/HWIO.ELF write /media/usb/'+test_name+' '+nonce+' for the provisioned test file.\n'
    'After checked shutdown and a fresh boot, run the same command with verify instead of write.\n'
    'Write+fsync alone is not proof of fresh-media readback. Preserve the accepted read-only desktop payload.\n')
media=str(out/'probe.raw')+'@@1048576'
run('mmd','-i',media,'::BROOK-TEST')
files={}
for p in sorted((out/'payload').rglob('*')):
    if not p.is_file():continue
    name=p.relative_to(out/'payload').as_posix();run('mcopy','-o','-i',media,p,'::'+name)
    files[name]={'bytes':p.stat().st_size,'sha256':sha(p)}
meta.update(files=files,kernel_sha256=files['KERNEL/BROOK.ELF']['sha256'],test_disk=str(out/'probe.raw'),
    usb_read_only=False,scope='Explicit writable scratch-test profile; no USB application-image mount',
    scratch_nonce=nonce,scratch_path='/media/usb/'+test_name,mutable_files=[test_name],
    physical_status='Prepared only; do not stage before tests pass')
for field in ['desktop_acceptance','usb_ecm_acceptance']:meta.pop(field,None)
(out/'manifest.json').write_text(json.dumps(meta,indent=2)+'\n')
print('HARDWARE_STORAGE_TEST_PREPARED',out)
