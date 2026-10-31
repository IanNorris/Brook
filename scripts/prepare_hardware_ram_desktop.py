#!/usr/bin/env python3
"""Freeze a remotely uploadable kernel/initrd desktop with RAM applications.

No physical-device access or new application-image copy. The accepted USB
APPS.BRK is snapshotted and SHA-256 verified at boot; no mutable USB application
source remains open. Root recovery files are not modified. Home/settings stay
volatile. Build the kernel/xHCI/EEVDF modules before running inside nix-shell.
"""
import argparse,hashlib,json,os,re,shutil,subprocess,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--recovery-probe',type=Path,required=True);p.add_argument('--maintenance-probe',type=Path,required=True)
p.add_argument('--output',type=Path,required=True);p.add_argument('--build',type=Path,default=ROOT/'build/release')
p.add_argument('--waylandd',type=Path,help='Stage a tested replacement provider in the initrd; its runtime closure must already exist in APPS.BRK')
p.add_argument('--display-idle-seconds',type=int,help='Opt in after physical panel/wake acceptance; 0 disables, maximum86400')
a=p.parse_args();recovery=a.recovery_probe.resolve(strict=True);maintenance=a.maintenance_probe.resolve(strict=True);build=a.build.resolve(strict=True);out=a.output.resolve()
if not os.environ.get('MUSL_CC'):p.error('Run in nix-shell with MUSL_CC')
if a.display_idle_seconds is not None and not 0<=a.display_idle_seconds<=86400:p.error('Invalid display idle timeout')
def sha(path):
    with path.open('rb') as source:return hashlib.file_digest(source,'sha256').hexdigest()
def run(*argv):subprocess.run(list(map(str,argv)),check=True,cwd=ROOT,stdin=subprocess.DEVNULL)
meta=json.loads((recovery/'manifest.json').read_text());mm=json.loads((maintenance/'manifest.json').read_text())
if meta.get('usb_read_only') is not True or mm.get('usb_read_only') is not False:p.error('Expected accepted RO desktop and RW maintenance inputs')
for name in ['APPS.BRK','INITRD.IMG']:
    path=recovery/'payload'/name;row=meta['files'][name]
    if not path.is_file() or path.is_symlink() or path.stat().st_size!=row['bytes'] or sha(path)!=row['sha256']:p.error('Invalid recovery input: '+name)
key=Path(mm['update_key_file']).read_text()
if not re.fullmatch('[0-9a-f]{32}',key):p.error('Invalid provisioned maintenance key')
for path in [build/'kernel/BROOK.elf',build/'kernel/drivers/xhci.mod',build/'kernel/drivers/sched_eevdf.mod',build/'kernel/drivers/virtio_input.mod']:
    if not path.is_file():p.error('Build dependency missing: '+str(path))
out.mkdir(parents=True,exist_ok=False)
run('cp','--reflink=auto',build/'kernel/BROOK.elf',out/'kernel.elf')
initrd=out/'INITRD.IMG';run('cp','--reflink=auto',recovery/'payload/INITRD.IMG',initrd)
# Launcher rebuilds distinguish an empty shortcuts directory from a vanished
# source. Retain the empty source even when this desktop uses only Nix entries.
if subprocess.run(['mdir','-i',str(initrd),'::BOOT/SHORTCUTS'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode:
    run('mmd','-i',initrd,'::BOOT/SHORTCUTS')
for name in ['usb-read-only','boot-app-image']:run('mdel','-i',initrd,'::ETC/'+name)
for name in ['xhci','sched_eevdf','virtio_input']:run('mcopy','-o','-i',initrd,build/'kernel/drivers'/f'{name}.mod','::DRIVERS/'+name.upper()+'.MOD')
for name,source in [('HWSTATUS.ELF','tools/hardware_status.c'),('HWUPDATE.ELF','tools/hardware_update.c'),('HWIO.ELF','tools/hardware_storage_probe.c'),('HWBULK.ELF','tools/hardware_bulk.c')]:
    run(os.environ['MUSL_CC'],'-static','-O2','-Wall','-Wextra','-Werror',ROOT/source,'-o',out/name)
    run('mcopy','-o','-i',initrd,out/name,'::BOOT/'+name)
musl_cxx=os.environ.get('MUSL_CXX') or os.environ['MUSL_CC'].removesuffix('gcc')+'g++'
run(musl_cxx,'-static','-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
    ROOT/'tools/hardware_store_receiver.cpp',ROOT/'tools/hardware_store_transaction.cpp',
    ROOT/'tools/hardware_store_validate.cpp','-o',out/'HWSTORE.ELF')
run('mcopy','-o','-i',initrd,out/'HWSTORE.ELF','::BOOT/HWSTORE.ELF')
apps=meta['files']['APPS.BRK'];payload_id='desktop-ram-apps-'+uuid.uuid4().hex
markers={'hardware-maintenance':'RAM-resident updater enabled; application snapshot must remain immutable\n',
    'hardware-bulk':'1\n',
    'hardware-update-key':key,'hardware-payload-id':payload_id+'\n',
    'boot-app-snapshot':f"/media/usb/APPS.BRK /nix {apps['bytes']} {apps['sha256']}\n"}
if a.display_idle_seconds is not None:markers['display-idle-seconds']=str(a.display_idle_seconds)+'\n'
for name,text in markers.items():
    path=out/name;path.write_text(text)
    if name=='hardware-update-key':path.chmod(0o600)
    run('mcopy','-o','-i',initrd,path,'::ETC/'+name)
provider='/nix/profile/bin/waylandd';provider_meta=None
if a.waylandd:
    binary=a.waylandd.resolve(strict=True)
    if not binary.is_file() or binary.read_bytes()[:4]!=b'\x7fELF':p.error('Replacement provider must be an ELF file')
    run('cp',binary,out/'waylandd')
    run('mcopy','-o','-i',initrd,out/'waylandd','::BOOT/waylandd')
    provider='/boot/waylandd';provider_meta={'source':str(binary),'sha256':sha(binary)}
rc=out/'INIT.RC';rc.write_text('set wm\nset vfb none\nsched eevdf\nkconsole\n'
    f'run --service --once --uid 1000 {provider}\n'
    'run --service --once --uid 1000 /nix/profile/bin/hardware-dbus\n'
    'run --service --once --uid 1000 /nix/profile/bin/hardware-message\n'
    'run --console --uid 1000 /boot/HWSTATUS.ELF\n')
run('mcopy','-o','-i',initrd,rc,'::BOOT/INIT.RC')
files={name:{'bytes':(out/name).stat().st_size,'sha256':sha(out/name)} for name in ['kernel.elf','INITRD.IMG']}
if files['kernel.elf']['bytes']>8*1024**2 or files['INITRD.IMG']['bytes']>64*1024**2:p.error('Upload exceeds bootstrap slot capacities')
manifest={'scope':'Opt-in RAM application desktop; kernel/initrd upload only','payload_id':payload_id,'files':files,
    'application_source':'/media/usb/APPS.BRK','application_image':apps,'application_snapshot_limit_bytes':512*1024**2,
    'initial_headroom_bytes':256*1024**2,'update_key_file':str(out/'hardware-update-key'),'recovery_probe':str(recovery),
    'maintenance_probe':str(maintenance),'physical_status':'Prepared; QEMU and physical acceptance required',
    'ephemeral_home':True,'usb_read_only':False,'mutable_usb_application_mount':False,
    'display_idle_seconds':a.display_idle_seconds or 0}
if provider_meta:manifest['wayland_provider_override']=provider_meta
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('HARDWARE_RAM_DESKTOP_PREPARED',out)
