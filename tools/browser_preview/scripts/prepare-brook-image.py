#!/usr/bin/env python3
"""Build a public-data-only Brook boot preview; run inside Brook's nix-shell."""
import bz2,gzip,hashlib,json,os,pathlib,shutil,subprocess,tempfile
PREVIEW=pathlib.Path(__file__).resolve().parents[1]
if os.environ.get('BROOK_ROOT'):ROOT=pathlib.Path(os.environ['BROOK_ROOT']).resolve()
else:
 ROOT=next((candidate for parent in pathlib.Path(__file__).resolve().parents for candidate in (parent,parent/'Brook') if (candidate/'src/kernel').is_dir()),None)
 if ROOT is None:raise RuntimeError('Set BROOK_ROOT to the Brook checkout')
OUT=PREVIEW/'dist/images';OUT.mkdir(parents=True,exist_ok=True)
BUILD=ROOT/'build/release'
def run(*args):return subprocess.run([str(x) for x in args],check=True,stdout=subprocess.DEVNULL)
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
with tempfile.TemporaryDirectory(prefix='brook-browser-image-') as temp:
 stage=pathlib.Path(temp);image=stage/'boot.img'
 # Build an isolated initrd from the same modules as the kernel image. Never
 # reuse a possibly stale build/esp/INITRD.IMG or a provider/user payload.
 private_build=stage/'build';drivers=private_build/'kernel/drivers';drivers.mkdir(parents=True)
 for module in sorted((BUILD/'kernel/drivers').glob('*.mod')):shutil.copy2(module,drivers/module.name)
 env=dict(os.environ,BROOK_BUILD_DIR=str(private_build));env.pop('BROOK_PROVIDER_PAYLOAD',None)
 subprocess.run(['bash',str(ROOT/'scripts/make_initrd.sh'),'--release'],env=env,check=True,stdout=subprocess.DEVNULL)
 initrd=private_build/'esp/INITRD.IMG'
 with image.open('wb') as f:f.truncate(64*1024*1024)
 run('mkfs.fat','-F','16','-n','BROOKWEB',image)
 for directory in ('EFI','EFI/BOOT','KERNEL','DRIVERS','SHORTCUTS'):
  run('mmd','-i',image,'::'+directory)
 files={BUILD/'bootloader/BOOTX64.efi':'EFI/BOOT/BOOTX64.EFI',BUILD/'kernel/BROOK.elf':'KERNEL/BROOK.ELF',initrd:'INITRD.IMG',ROOT/'data/wallpaper.raw':'WALLPAPER.RAW'}
 for source in sorted((BUILD/'kernel/drivers').glob('*.mod')):files[source]='DRIVERS/'+source.name.upper()
 texts={'BROOK.MNT':'/boot','BROOK.CFG':'TARGET=KERNEL\\BROOK.ELF\nDEBUG_TEXT=0\n','INIT.RC':'display 1024x768\nset wm\nset vfb none\nkconsole\n','SHORTCUTS/LOGS.RC':'# title: Kernel Console\n# Requires: builtin\nkconsole\n'}
 for name,value in texts.items():
  source=stage/name.replace('/','_');source.write_text(value);files[source]=name
 for source,dest in files.items():
  run('mcopy','-o','-i',image,source,'::'+dest)
  data=subprocess.check_output(['mtype','-i',str(image),'::'+dest])
  if hashlib.sha256(data).hexdigest()!=digest(source):raise RuntimeError('FAT readback mismatch: '+dest)
 run('fsck.fat','-n',image)
 with image.open('rb') as src,(OUT/'brook-boot.img.gz').open('wb') as raw:
  with gzip.GzipFile(filename='',mode='wb',fileobj=raw,mtime=0,compresslevel=9) as target:shutil.copyfileobj(src,target)
 manifest={'kernelSha256':digest(BUILD/'kernel/BROOK.elf'),'sourceRevision':subprocess.check_output(['git','-C',ROOT,'rev-parse','HEAD'],text=True).strip(),'sourceDirty':bool(subprocess.check_output(['git','-C',ROOT,'status','--porcelain'],text=True).strip()),'rawDiskBytes':image.stat().st_size,'compressedDiskBytes':(OUT/'brook-boot.img.gz').stat().st_size,'purpose':'Initial real-kernel boot/display/input preview. No Linux application closure or private media.','files':{dest:{'bytes':src.stat().st_size,'sha256':digest(src)} for src,dest in files.items()}}
 for name,env in [('OVMF_CODE.fd','OVMF_CODE'),('OVMF_VARS.fd','OVMF_VARS')]:
  source=pathlib.Path(os.environ[env]);shutil.copyfile(source,OUT/name);manifest[name]={'bytes':source.stat().st_size,'sha256':digest(source),'source':str(source)}
 qemu=pathlib.Path(shutil.which('qemu-system-x86_64')).resolve()
 for name in ('vgabios-stdvga.bin','kvmvapic.bin'):
  bios=qemu.parent.parent/'share/qemu'/name
  if not bios.exists():raise RuntimeError('Missing QEMU firmware '+str(bios))
  shutil.copyfile(bios,OUT/name);manifest[name]={'sha256':digest(bios),'bytes':bios.stat().st_size}
 # Archived compressed inputs make the compatibility firmware reproducible
 # without relying on the host OVMF version or a mutable download URL.
 fallback=pathlib.Path(__file__).resolve().parents[1]/'dist/runtime/source/fallback-firmware-inputs'
 inputs=json.loads((fallback/'inputs.json').read_text())
 fallback_manifest={}
 for original,name in [('edk2-x86_64-code.fd.bz2','OVMF_202302_CODE.fd'),('edk2-i386-vars.fd.bz2','OVMF_202302_VARS.fd')]:
  source=fallback/original
  if digest(source)!=inputs['files'][original]['sha256']:raise RuntimeError('Fallback firmware checksum mismatch: '+original)
  data=bz2.decompress(source.read_bytes());(OUT/name).write_bytes(data)
  details={'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),'firmwareTag':inputs['firmwareTag'],'source':inputs['sourceRepository']+'/blob/'+inputs['sourceCommit']+'/pc-bios/'+original}
  manifest[name]=details;fallback_manifest[name]=details
 # QEMU maps flash0 CODE at the top of4GiB, then flash1 VARS below it.
 # A ROM image preserves that exact mapping by concatenating VARS then CODE.
 rom=(OUT/'OVMF_202302_VARS.fd').read_bytes()+(OUT/'OVMF_202302_CODE.fd').read_bytes()
 if len(rom)!=4*1024*1024:raise RuntimeError('Combined OVMF ROM is not4MiB')
 (OUT/'OVMF_202302_ROM.fd').write_bytes(rom)
 details={'bytes':len(rom),'sha256':hashlib.sha256(rom).hexdigest(),'firmwareTag':inputs['firmwareTag'],'layout':['OVMF_202302_VARS.fd','OVMF_202302_CODE.fd'],'physicalBase':'0xffc00000','mode':'Read-onlyROM via QEMU -bios; volatile/emulated UEFI variables'}
 manifest['OVMF_202302_ROM.fd']=details;fallback_manifest['OVMF_202302_ROM.fd']=details
 shutil.copyfile(fallback/'edk2-licenses.txt',OUT/'edk2-202302-licenses.txt')
 (OUT/'firmware-fallback.json').write_text(json.dumps(fallback_manifest,indent=2)+'\n')
 shutil.copyfile(ROOT/'LICENSE',OUT/'BROOK-LICENSE.txt')
 (OUT/'brook-image.json').write_text(json.dumps(manifest,indent=2)+'\n')
 print(json.dumps({'image':str(OUT/'brook-boot.img.gz'),'compressedBytes':manifest['compressedDiskBytes'],'kernelSha256':manifest['kernelSha256']}))
