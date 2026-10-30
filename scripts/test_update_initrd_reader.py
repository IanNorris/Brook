#!/usr/bin/env python3
"""Read exact metadata from real FAT12/16/32 initrds; never change input bytes."""
import argparse,hashlib,json,struct,subprocess,time
from pathlib import Path
from build_update_initrd_inspector import compile_inspector
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);binary=compile_inspector(out/'build')
def run(*argv):subprocess.run(list(map(str,argv)),check=True,cwd=ROOT,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL)
def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def inspect(image,path,code=0,text=None,timeout=5):
    before=sha(image);started=time.monotonic();result=subprocess.run([str(binary),str(image),path],capture_output=True,timeout=timeout)
    assert result.returncode==code,(image.name,path,result.returncode,result.stderr)
    if text is not None:assert result.stdout==text,(result.stdout,text)
    assert sha(image)==before,'Read-only inspector modified '+str(image)
    return time.monotonic()-started
marker=out/'boot-app-snapshot';content=b'/media/usb/UPDATES/B/APPS.BRK /nix 262602752 '+b'7'*64+b'\n';marker.write_bytes(content)
init=out/'INIT.RC';init.write_bytes(b'set wm\nrun /boot/HWSTATUS.ELF\n')
large=out/'hardware-maintenance';large.write_bytes(b'x'*4097)
results=[]
for fat,mib in [(12,2),(16,16),(32,64)]:
    image=out/f'fat{fat}.img'
    with image.open('wb') as f:f.truncate(mib*1024*1024)
    run('mkfs.fat','-F',str(fat),image)
    run('mmd','-i',image,'::ETC','::BOOT')
    run('mcopy','-i',image,marker,'::ETC/boot-app-snapshot');run('mcopy','-i',image,init,'::BOOT/INIT.RC')
    run('mcopy','-i',image,large,'::ETC/hardware-maintenance')
    inspect(image,'0:/ETC/boot-app-snapshot',text=content)
    inspect(image,'0:/BOOT/INIT.RC',text=init.read_bytes())
    inspect(image,'0:/ETC/boot-app-image',11)
    inspect(image,'0:/ETC/hardware-maintenance',13)
    inspect(image,'0:/ETC/../BOOT/INIT.RC',12)
    results.append({'fat':fat,'passed':True,'exact_lfn_metadata':True,'input_unchanged':True})
# The FAT32 root directory is made cyclic with no end marker. The bounded RAM
# backend/upstream directory bound must return no metadata instead of hanging.
# Missing is a lookup result, not a claim that the entire FAT volume is sound.
cycle=out/'cyclic.img';run('cp','--reflink=auto',out/'fat32.img',cycle)
with cycle.open('r+b') as f:
    boot=f.read(512);reserved=struct.unpack_from('<H',boot,14)[0];fats=boot[16];sectors_per_fat=struct.unpack_from('<I',boot,36)[0]
    root=struct.unpack_from('<I',boot,44)[0];data_sector=reserved+fats*sectors_per_fat+(root-2)*boot[13]
    for copy in range(fats):f.seek((reserved+copy*sectors_per_fat)*512+root*4);f.write(struct.pack('<I',root))
    entry=bytearray(32);entry[:11]=b'NOTMATCHBIN';entry[11]=0x20
    f.seek(data_sector*512);f.write(bytes(entry)*(512*boot[13]//32))
cycle_elapsed=inspect(cycle,'0:/ETC/boot-app-snapshot',11,timeout=10)
# A partial image cannot be accepted just because its root metadata survived.
short=out/'truncated.img';run('cp','--reflink=auto',out/'fat32.img',short)
with short.open('r+b') as f:f.truncate(1024*1024)
inspect(short,'0:/ETC/boot-app-snapshot',12)
bad=out/'bad-boot.img';run('cp','--reflink=auto',out/'fat16.img',bad)
with bad.open('r+b') as f:f.seek(510);f.write(b'\0\0')
inspect(bad,'0:/ETC/boot-app-snapshot',12)
# Also inspect the actual prepared desktop initrd with its real long filename.
actual=ROOT/'build/hardware/ram-desktop-update-20261030'
if actual.is_dir():inspect(actual/'INITRD.IMG','0:/ETC/boot-app-snapshot',text=(actual/'boot-app-snapshot').read_bytes())
(out/'result.json').write_text(json.dumps({'passed':True,'scope':'RAM-only read-only initrd metadata','cases':results,
    'cyclic_directory_no_metadata_seconds':cycle_elapsed,'truncated_volume_rejected':True,'bad_boot_rejected':True},indent=2)+'\n')
print('UPDATE_INITRD_READER_PASS')
