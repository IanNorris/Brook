#!/usr/bin/env python3
"""Fetch pinned public emulator/firmware inputs; no guest/user disks are read."""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess,urllib.request,uuid
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--alpine-control',action='store_true');parser.add_argument('--diagnostics',action='store_true');args=parser.parse_args()
runtime=root/'dist/runtime';(runtime/'source').mkdir(parents=True,exist_ok=True)
checks={line.split()[1]:line.split()[0] for line in (root/'metadata/UPSTREAM-SHA256SUMS').read_text().splitlines()}
def fetch(url,dest,expected,limit=32*1024*1024):
 dest.parent.mkdir(parents=True,exist_ok=True)
 if dest.exists():
  if hashlib.sha256(dest.read_bytes()).hexdigest()!=expected:raise RuntimeError('Existing input checksum mismatch: '+str(dest))
  return
 temp=dest.with_name(dest.name+'.download-'+uuid.uuid4().hex)
 try:
  h=hashlib.sha256();size=0
  with urllib.request.urlopen(url,timeout=30) as response,temp.open('xb') as out:
   while data:=response.read(1024*1024):
    size+=len(data)
    if size>limit:raise RuntimeError('Download exceeds input bound')
    h.update(data);out.write(data)
  if h.hexdigest()!=expected:raise RuntimeError('Downloaded checksum mismatch: '+url)
  temp.rename(dest)
 finally:temp.unlink(missing_ok=True)
base='https://dbontr.github.io/linux-lab/qemu/'
for source,dest in [('out.js',root/'vendor/linux-lab-runtime/out.js'),('qemu-system-x86_64.wasm',root/'vendor/linux-lab-runtime/qemu-system-x86_64.wasm'),('qemu-system-x86_64.worker.js',root/'vendor/linux-lab-runtime/qemu-system-x86_64.worker.js'),('COPYING.qemu',root/'vendor/linux-lab-runtime/COPYING.qemu'),('LICENSE.sdl',root/'vendor/linux-lab-runtime/LICENSE.sdl')]:fetch(base+source,dest,checks[source])
shutil.copyfile(root/'metadata/UPSTREAM-SHA256SUMS',runtime/'source/UPSTREAM-SHA256SUMS')
firmware=json.loads((root/'metadata/firmware-inputs.json').read_text());out=runtime/'source/fallback-firmware-inputs';out.mkdir(exist_ok=True)
(out/'inputs.json').write_text(json.dumps(firmware,indent=2)+'\n')
for name,entry in firmware['files'].items():
 url='https://raw.githubusercontent.com/zb3/qemu-wasm-test/'+firmware['sourceCommit']+'/pc-bios/'+name
 fetch(url,out/name,entry['sha256'])
if args.alpine_control:
 name='alpine-virt-3.24.1-x86_64.iso'
 fetch('https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/x86_64/'+name,root/'dist/images/controls'/name,'e73a6241bd5f3c5c2d4d38c02cc52c378c0415a7c888bd292066bf36e0f41a39',100*1024*1024)
for script in ['prepare-runtime.py']:
 subprocess.run(['python3',str(root/'scripts'/script)],check=True)
for script in ['prepare-pollfix-runtime.mjs','prepare-exitfix-runtime.mjs']+(['prepare-chainfix-runtime.mjs','prepare-nochain-runtime.mjs'] if args.diagnostics else []):subprocess.run(['node',str(root/'scripts'/script)],check=True)
print('Pinned runtime and firmware inputs ready. Build the public Brook image next.')
