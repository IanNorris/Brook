#!/usr/bin/env python3
"""Read-only compressed application file on a FAT32/MBR USB disk, under QEMU.

Requires nix-shell, built kernel/bootloader/xHCI and host checksum helper.
Only creates disposable images. No physical disk, desktop or audio access.
"""
import argparse, hashlib, json, os, shutil, socket, subprocess, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--output',required=True,type=Path)
args=ap.parse_args();base=args.output.resolve();base.mkdir(parents=True,exist_ok=False)
def run(*cmd,**kw):return subprocess.run(list(map(str,cmd)),check=True,cwd=ROOT,**kw)
source=base/'source';source.mkdir();(source/'private').mkdir(mode=0o700)
run(os.environ['MUSL_CC'],'-static','-O2','-Wall','-Wextra','-Werror',ROOT/'scripts/workloads/compressed_image_elf.c','-o',source/'hello')
(source/'data.bin').write_bytes(bytes((i*73+(i>>8))&255 for i in range(131089)))
os.link(source/'data.bin',source/'hard');(source/'link').symlink_to('data.bin')
(source/'private/denied').write_text('private');shutil.copy2(source/'hello',source/'private/hello')
# Uncompressible padding makes the source exceed FAT's old preload threshold.
(source/'padding').write_bytes(os.urandom(2*1024*1024))
image=base/'APPS.BRK';env=dict(os.environ,BROOK_IMAGE_CHECKSUM_LIBRARY=str(ROOT/'build/release/host_tests/libbrook_image_checksum.so'))
run('python3',ROOT/'scripts/compressed_image.py','build',source,image,env=env)
probe=base/'probe'
run('python3',ROOT/'scripts/make_uefi_probe.py','--output',probe,'--driver','xhci','--usb-read-only','--media-mount','/media/usb','--partition-table','mbr','--display-width','800','--display-height','600','--application-image',image,'--volatile-desktop')
rc=base/'INIT.RC';rc.write_text('set wm\nset vfb none\nkconsole\nrun --uid 0 /nix/hello /nix --detach /media/usb\n')
initrd=probe/'payload/INITRD.IMG';run('mcopy','-o','-i',initrd,rc,'::BOOT/INIT.RC')
run('mcopy','-o','-i',str(probe/'probe.raw')+'@@1048576',initrd,'::INITRD.IMG')
variables=base/'VARS.fd';shutil.copy2(os.environ['OVMF_VARS'],variables);variables.chmod(0o600)
qemu=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','4','-m','512','-nodefaults',
 '-drive','if=pflash,format=raw,readonly=on,file='+os.environ['OVMF_CODE'],'-drive','if=pflash,format=raw,file='+str(variables),
 '-device','VGA','-device','qemu-xhci,id=usbc','-drive','if=none,id=stick,format=raw,readonly=on,file='+str(probe/'probe.raw'),
 '-device','usb-storage,bus=usbc.0,port=1,drive=stick,bootindex=1','-display','none',
 '-serial','file:'+str(base/'serial.log'),'-qmp','unix:'+str(base/'qmp.sock')+',server=on,wait=off','-no-reboot']
(base/'argv.json').write_text(json.dumps(qemu,indent=2)+'\n')
with (base/'qemu.log').open('x') as log:
 owner=subprocess.Popen(qemu,stdout=log,stderr=subprocess.STDOUT)
 try:
  deadline=time.monotonic()+120
  while time.monotonic()<deadline:
   assert owner.poll() is None,'QEMU ended early'
   serial=(base/'serial.log').read_text(errors='replace') if (base/'serial.log').exists() else ''
   assert 'PANIC_HEX:' not in serial and 'Assertion failed' not in serial,serial[-2500:]
   if 'COMPRESSED_IMAGE_ELF_PASS' in serial:break
   time.sleep(.1)
  else:raise TimeoutError(serial[-3500:])
  assert 'brookimg: application file ready' in serial
  with socket.socket(socket.AF_UNIX) as q:
   q.settimeout(10);q.connect(str(base/'qmp.sock'));f=q.makefile('rwb',buffering=0);f.readline()
   for command in ['qmp_capabilities','quit']:
    f.write((json.dumps({'execute':command})+'\n').encode())
    while True:
     answer=json.loads(f.readline())
     if 'return' in answer:break
     if 'error' in answer:raise RuntimeError(answer)
  assert owner.wait(timeout=15)==0
  (base/'result.json').write_text(json.dumps(dict(passed=True,ram_mib=512,source_bytes=image.stat().st_size,sha256=hashlib.sha256(image.read_bytes()).hexdigest(),checks=['MBR USB','intrinsic readonly FAT','streaming compressed file','ELF execution','cross-block reads','hardlinks','symlinks','private COW','permissions','source and image detach with retained handles']),indent=2)+'\n')
  print('USB_APP_IMAGE_ACCEPTANCE_PASS',base,flush=True)
 finally:
  if owner.poll() is None:
   owner.terminate()
   try:owner.wait(timeout=10)
   except subprocess.TimeoutExpired:owner.kill();owner.wait()
