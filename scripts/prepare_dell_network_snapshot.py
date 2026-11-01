#!/usr/bin/env python3
"""Prepare a private diagnostic kernel/initrd pair; never writes a device."""
import argparse,hashlib,json,re,shutil,subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--base',type=Path,required=True,help='Existing Dell candidate directory')
p.add_argument('--output',type=Path,required=True)
p.add_argument('--payload-id',required=True,help='New hardware payload identifier')
p.add_argument('--tx-depth16',action='store_true',help='Opt in to sixteen USB ECM transmit slots')
p.add_argument('--tcp-window16',action='store_true',help='Opt in to a 16 KiB TCP receive-window cap')
p.add_argument('--build',type=Path,default=ROOT/'build/release')
a=p.parse_args();base=a.base.resolve(strict=True);build=a.build.resolve(strict=True)
if not re.fullmatch(r'[a-zA-Z0-9_.-]{1,95}',a.payload_id):p.error('Invalid payload identifier')
out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
image=out/'INITRD.IMG';shutil.copyfile(base/'INITRD.IMG',image)
shutil.copyfile(build/'kernel/BROOK.elf',out/'kernel.elf')
for name in ('xhci','sched_eevdf','virtio_input'):
    source=build/'kernel/drivers'/f'{name}.mod'
    subprocess.run(['mcopy','-o','-i',str(image),str(source),f'::DRIVERS/{name.upper()}.MOD'],check=True)
identity=a.payload_id;identity_file=out/'hardware-payload-id'
identity_file.write_text(identity+'\n')
subprocess.run(['mcopy','-o','-i',str(image),str(identity_file),'::ETC/hardware-payload-id'],check=True)
if a.tx_depth16:
    marker=out/'usb-net-tx16';marker.write_text('1\n')
    subprocess.run(['mcopy','-o','-i',str(image),str(marker),'::ETC/usb-net-tx16'],check=True)
if a.tcp_window16:
    marker=out/'tcp-recv-window-16k';marker.write_text('1\n')
    subprocess.run(['mcopy','-o','-i',str(image),str(marker),'::ETC/tcp-recv-window-16k'],check=True)
rows={}
for name in ('kernel.elf','INITRD.IMG'):
    path=out/name
    with path.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
    rows[name]={'bytes':path.stat().st_size,'sha256':digest}
abi=re.search(r'MODULE_ABI_VERSION\s*=\s*(\d+)',(ROOT/'src/kernel/src/module_abi.h').read_text())
if not abi:raise RuntimeError('Cannot determine current module ABI')
(out/'manifest.json').write_text(json.dumps({'payload_id':identity,'module_abi':int(abi[1]),
    'base':str(base),'files':rows,'physical_status':'prepared only; not installed'},indent=2)+'\n')
print('DELL_NETWORK_SNAPSHOT_PREPARED',out)
