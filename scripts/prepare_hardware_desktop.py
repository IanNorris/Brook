#!/usr/bin/env python3
"""Prepare an offline, read-only USB desktop payload; never write physical media.

Build nix/hardware-packages.nix -A profile first, then run inside nix-shell.
The generated desktop stores home/settings in RAM and loses them at shutdown.
"""
import argparse, json, os, shutil, struct, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--profile',type=Path,default=ROOT/'build/hardware/offline-profile')
ap.add_argument('--output',type=Path,required=True)
ap.add_argument('--width',type=int,default=3840)
ap.add_argument('--height',type=int,default=2160)
args=ap.parse_args();profile=args.profile.resolve(strict=True);base=args.output.resolve()
if profile.parent!=Path('/nix/store'):ap.error('Profile must resolve to a Nix store path')
base.mkdir(parents=True,exist_ok=False)
def run(*cmd,**kw):return subprocess.run(list(map(str,cmd)),check=True,cwd=ROOT,**kw)
closure=subprocess.check_output(['nix-store','-qR',str(profile)],text=True).splitlines()
if not closure or any(Path(p).parent!=Path('/nix/store') or not Path(p).is_dir() for p in closure):raise RuntimeError('Invalid Nix closure')
(base/'closure.txt').write_text('\n'.join(closure)+'\n')
source=base/'source';(source/'store').mkdir(parents=True)
for item in closure:shutil.copytree(item,source/'store'/Path(item).name,symlinks=True)
(source/'profile').symlink_to('store/'+profile.name);(source/'bin').symlink_to('profile/bin')
(source/'etc/ssl/certs').mkdir(parents=True)
(source/'etc/ssl/certs/ca-bundle.crt').symlink_to('/nix/profile/etc/ssl/certs/ca-bundle.crt')
run('python3',ROOT/'scripts/collect_desktop_entries.py',source/'store',source/'share/applications')
# Route installed GUI entries through a reproducible environment wrapper.
manifest=source/'share/applications/applications.idx'
records=[]
for record in manifest.read_text().splitlines():
 fields=record.split('\t');command=fields[1]
 for name in ['mousepad','brook-files','brook-settings','wayland-calc']:
  if command.split(' ',1)[0] in [name,'/nix/profile/bin/'+name]:fields[1]='/nix/profile/bin/hardware-'+name
 records.append('\t'.join(fields))
manifest.write_text('\n'.join(records)+'\n')
image=base/'APPS.BRK';env=dict(os.environ,BROOK_IMAGE_CHECKSUM_LIBRARY=str(ROOT/'build/release/host_tests/libbrook_image_checksum.so'))
run('python3',ROOT/'scripts/compressed_image.py','build',source,image,env=env)
if image.stat().st_size>=1024**3:raise RuntimeError('Hardware application image exceeds 1 GiB cap')
# Whole closure copy is build scratch, not another permanent disk image.
for directory,_,_ in os.walk(source):os.chmod(directory,0o755)
shutil.rmtree(source)
probe=base/'probe'
env=dict(os.environ,BROOK_INITRD_KIB='32768')
run('python3',ROOT/'scripts/make_uefi_probe.py','--output',probe,'--driver','xhci','--driver','sched_eevdf','--usb-read-only','--media-mount','/media/usb','--partition-table','mbr','--display-width',args.width,'--display-height',args.height,'--application-image',image,'--volatile-desktop',env=env)
initrd=probe/'payload/INITRD.IMG'
run(os.environ['MUSL_CC'],'-static','-O2','-Wall','-Wextra','-Werror',ROOT/'tools/hardware_status.c','-o',base/'HWSTATUS.ELF')
run(os.environ['MUSL_CC'],'-static','-O2','-Wall','-Wextra','-Werror',ROOT/'src/tests/test_usb_storage_guest/main.c','-o',base/'USBPROBE.ELF')
for name in ['HWSTATUS.ELF','USBPROBE.ELF']:run('mcopy','-i',initrd,base/name,'::BOOT/'+name)
fixture=probe/'payload/READTEST.BIN';pattern=bytes((i*17+5)%251 for i in range(251))
with fixture.open('xb') as output:
    remaining=8*1024*1024
    while remaining:
        chunk=(pattern*4096)[:min(remaining,251*4096)];output.write(chunk);remaining-=len(chunk)
run('mcopy','-i',str(probe/'probe.raw')+'@@1048576',fixture,'::READTEST.BIN')
defaults=base/'desktop-defaults.ini';defaults.write_text('[Desktop]\nscale='+('200' if args.width>2560 else '100')+'\n')
run('mcopy','-i',initrd,defaults,'::ETC/desktop-defaults.ini')
run('mmd','-i',initrd,'::BOOT/BIN')
run('mcopy','-i',initrd,profile/'bin/bash','::BOOT/BIN/BASH')
from PIL import Image, ImageOps
wallpaper=ImageOps.fit(Image.open(ROOT/'data/wallpaper/brook-dawn.png').convert('RGB'),(1920,1080))
raw=base/'WALLPAPER.RAW';raw.write_bytes(struct.pack('<II',1920,1080)+wallpaper.convert('RGBA').tobytes('raw','BGRA'))
run('mcopy','-i',initrd,raw,'::BOOT/WALLPAPER.RAW');raw.unlink()
rc=base/'INIT.RC';rc.write_text('set wm\nset vfb none\nsched eevdf\nkconsole\n'
 'run --service --once --uid 1000 /nix/profile/bin/waylandd\n'
 'run --service --once --uid 1000 /nix/profile/bin/hardware-dbus\n'
 'run --service --once --uid 1000 /nix/profile/bin/hardware-message\n'
 'run --uid 1000 /boot/HWSTATUS.ELF\n'
 'run --uid 1000 /boot/USBPROBE.ELF /media/usb/READTEST.BIN --concurrent\n')
run('mcopy','-o','-i',initrd,rc,'::BOOT/INIT.RC')
readme=probe/'payload/README.TXT'
readme.write_text("Brook Dell XPS 13 9343 offline hardware desktop\n\n"
    "Boot the UEFI USB entry with Secure Boot disabled. No installation or disk formatting is performed.\n"
    "Applications are read from APPS.BRK on the existing FAT32 stick. USB writes are disabled.\n"
    "Home, settings and temporary files are RAM ONLY and disappear at shutdown.\n"
    "Installed: Bash/coreutils, Vim, Mousepad, Files, Settings and Calculator. Super+Enter opens a terminal.\n"
    "Kernel Console shows USB_IO_PROBE_OK after six checked reads, plus HARDWARE_NETWORK_READY with the IP if Ethernet succeeds.\n"
    "Share the IP for the boot-pinned debug bridge on TCP1234. Physical Tecknet ECM networking still needs validation.\n"
    "Audio starts muted; the Dell internal SST/RT286 codec and native Intel GPU acceleration are not implemented.\n"
    "Recovery: the prior known-good payload is preserved on the build host; panic scanner https://192.168.85.8:9001/.\n")
# Keep this payload's metadata and its disposable QEMU disk identical.
import hashlib
meta=json.loads((probe/'manifest.json').read_text())
meta['files']['INITRD.IMG'].update(bytes=initrd.stat().st_size,sha256=hashlib.sha256(initrd.read_bytes()).hexdigest())
meta['files']['READTEST.BIN']=dict(bytes=fixture.stat().st_size,sha256=hashlib.sha256(fixture.read_bytes()).hexdigest())
meta['files']['README.TXT'].update(bytes=readme.stat().st_size,sha256=hashlib.sha256(readme.read_bytes()).hexdigest())
(probe/'manifest.json').write_text(json.dumps(meta,indent=2)+'\n')
run('mcopy','-o','-i',str(probe/'probe.raw')+'@@1048576',initrd,'::INITRD.IMG')
run('mcopy','-o','-i',str(probe/'probe.raw')+'@@1048576',readme,'::README.TXT')
(base/'desktop.json').write_text(json.dumps(dict(profile=str(profile),packages=len(closure),application_image_bytes=image.stat().st_size,ephemeral_home=True,network='physical ECM acceptance pending',audio='muted; internal codec not implemented'),indent=2)+'\n')
print('OFFLINE_HARDWARE_DESKTOP_PREPARED',probe/'payload',flush=True)
