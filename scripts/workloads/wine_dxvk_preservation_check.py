#!/usr/bin/env python3
# Run from the repository root; never mounts or modifies either guest disk.
from pathlib import Path
import json,subprocess,tempfile,hashlib,re,os,stat
import argparse
parser=argparse.ArgumentParser(description='Offline saved-data audit after a stopped Wine/DXVK acceptance run')
parser.add_argument('--candidate',required=True,type=Path)
parser.add_argument('--seed',required=True,type=Path)
args=parser.parse_args()
b=args.candidate.resolve();r=b.parent;seed=args.seed.resolve()
load=lambda p:json.loads(Path(p).read_text())
sha=lambda v:hashlib.sha256(v).hexdigest()
assert load(b/'acceptance-complete.json')['passed']
assert load(seed/'acceptance-audit.json')['passed']
assert load(b/'shutdown-result.json')['orderly_shutdown_completed'] and load(b/'shutdown-result.json')['qemu_exit_code']==0
assert load(b/'launcher-exit.json')['exit_code']==0 and all(v['exit']==0 for v in load(b/'after-fsck.json').values())
assert load(b/'heap-check.json')['reply']=='heap integrity: OK'
specs=load('build/workloads/wine-shared-mapping-20261019/candidate/before-documents.json')
specs += [dict(path='/data/home/gpu-workloads-2c277862.txt',sha256='e480ea550659af9886f324123da85e9f48a064645771062fe5479432f89a1c4d'),dict(path='/data/home/wine-shared-mapping-acceptance.bin',sha256='9d03969c5c281c0ca6ab96351ec4dad4d5a159d1bd1149916156b265b9c31ced'),dict(path='/data/home/wine-notepad-acceptance.txt',sha256='a695edf6085c6cbb55ca732727ad80a7d77f222240be561c4ac3919c2d879249')]
def debug(image,command):return subprocess.run(['debugfs','-R',command,str(image)],capture_output=True,text=True,check=True).stdout
def tree_manifest(root):
 result={}
 for p in sorted(root.rglob('*')):
  st=p.lstat();entry=dict(mode=stat.S_IMODE(st.st_mode))
  if p.is_symlink():entry.update(kind='symlink',target=os.readlink(p))
  elif p.is_file():entry.update(kind='file',size=st.st_size,sha256=sha(p.read_bytes()))
  elif p.is_dir():entry.update(kind='directory')
  else:raise RuntimeError(str(p))
  result[str(p.relative_to(root))]=entry
 return result
with tempfile.TemporaryDirectory(dir=r,prefix='audit-') as tmp:
 work=Path(tmp)
 for i,s in enumerate(specs):
  guest=s['path'].removeprefix('/data');out=work/str(i)
  assert re.search(r'User:\s+1000\s+Group:\s+100\b',debug(b/'run/data.img','stat '+json.dumps(guest)))
  debug(b/'run/data.img','dump '+json.dumps(guest)+' '+json.dumps(str(out)))
  assert sha(out.read_bytes())==s['sha256'],guest
 profiles={}
 for guest in ('/home/.config','/home/.mozilla','/home/.vscode-oss','/home/wine-acceptance/prefix-file-trace'):
  manifests=[]
  for n,base in enumerate((seed,b)):
   target=work/(str(n)+'-'+guest.split('/')[-1]);target.mkdir()
   debug(base/'run/data.img','rdump '+json.dumps(guest)+' '+json.dumps(str(target)))
   assert list(target.iterdir()),guest
   manifests.append(tree_manifest(target))
  assert manifests[0]==manifests[1],guest
  profiles[guest]=dict(entries=len(manifests[0]),manifest_sha256=sha(json.dumps(manifests[0],sort_keys=True).encode()))
 sources={name:sha((b/name).read_bytes()) for name in ('acceptance-driver.py','source.patch','packages.json','manifest.json')}
 (b/'preservation-audit.json').write_text(json.dumps(dict(passed=True,scope='saved-data and filesystem preservation only; Wine/DXVK workload acceptance audited separately',preserved_document_fixtures=len(specs),preserved_profiles=profiles,evidence_sha256=sources,checked_shutdown=True,offline_fsck_passed=True),indent=2)+'\n')
print('WINE_DXVK_PRESERVATION_AUDIT_PASS')
