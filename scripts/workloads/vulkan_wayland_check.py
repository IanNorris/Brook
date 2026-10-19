#!/usr/bin/env python3
from pathlib import Path
import sys,os,json,subprocess,shutil,time,uuid,struct,re
from elftools.elf.elffile import ELFFile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]));import bootstrap_acceptance as b
import argparse
parser=argparse.ArgumentParser(description='Private Venus native Wayland lifecycle and GL coexistence acceptance. Run inside nix-shell from the repository root.')
parser.add_argument('--output',required=True,type=Path)
parser.add_argument('--seed',required=True,type=Path,help='Checked seed directory containing run/ and artifacts/')
parser.add_argument('--nix-seed',required=True,type=Path,help='Offline Nix image containing the matching Mesa/tools closure')
parser.add_argument('--control',type=Path,default=Path('build/vulkan-triangle-control-result'))
parser.add_argument('--artifacts',type=Path,default=Path('build/release'))
parser.add_argument('--vkmark',type=Path,help='Optional vkmark package already staged on the private Nix seed')
parser.add_argument('--instance',type=int,default=510)
args=parser.parse_args()
r=args.output.resolve();r.mkdir(parents=True,exist_ok=False);run=r/'run';run.mkdir()
seed=args.seed.resolve();assert json.loads((seed/'shutdown-result.json').read_text())['orderly_shutdown_completed'];assert all(v['exit']==0 for v in json.loads((seed/'after-fsck.json').read_text()).values())
for p in Path('/proc').glob('[0-9]*/exe'):
 try:t=str(p.resolve(strict=True))
 except (OSError,RuntimeError):continue
 assert 'qemu-system' not in Path(t).name,p
assert b.memory_available()>8*1024**3
for name in ('boot','data','nix','usb','tmp'):subprocess.run(['cp','--reflink=auto','--sparse=always',str((args.nix_seed.resolve()) if name=='nix' else seed/'run'/(name+'.img')),str(run/(name+'.img'))],check=True)
art=r/'artifacts';shutil.copytree(seed/'artifacts',art)
for part in ('kernel/BROOK.elf','kernel/ksym_addr_table.cpp','bootloader/BOOTX64.efi',*[str(p.relative_to(args.artifacts)) for p in (args.artifacts/'kernel/drivers').glob('*.mod')]):shutil.copy2(args.artifacts/part,art/part)
abis={}
for module in (art/'kernel/drivers').glob('*.mod'):
 with module.open('rb') as stream:magic,abi=struct.unpack_from('<II',ELFFile(stream).get_section_by_name('.modinfo').data())
 assert magic==0x4b524f42 and abi==31;abis[module.name]=dict(abi=abi,sha256=b.sha(module))
assert len(abis)==12;b.write(r/'module-abi-audit.json',abis)
env=dict(os.environ,BROOK_BUILD_DIR=str(art),BROOK_DISK_IMG=str(run/'boot.img'));b.command([str(b.ROOT/'scripts/update_disk.sh')],r/'update-boot.log',env)
control=json.loads((args.control/'manifest.json').read_text());probe=Path(control['probe'])/'bin/virtgpu-context'
for name,target in [('virtgpu-context','VKCTX'),('virtgpu-query','VKQUERY'),('virtgpu-blob','VKBLOB'),('vulkan-probe','VKPROBE')]:subprocess.run(['mcopy','-o','-i',str(run/'boot.img'),str(probe.parent/name),'::/BIN/'+target],check=True)
b.write(r/'manifest.json',dict(revision=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),seed=str(seed),kernel_sha256=b.sha(art/'kernel/BROOK.elf'),probe_package=control['probe'],control_packages=control,before_fsck=b.offline_fsck(run,'before')))
for key,value in dict(BROOK_GPU_QEMU=control['qemu'],BROOK_GPU_MESA=control['mesa'],BROOK_GPU_GLVND=control['glvnd'],BROOK_GPU_VULKAN=control['vulkanLoader'],BROOK_GPU_RENDERER=control['virglrenderer'],BROOK_GPU_HOST_ICD=control['mesa']+'/share/vulkan/icd.d/intel_icd.x86_64.json',BROOK_GPU_HOSTMEM='1G',BROOK_RENDERNODE='/dev/dri/renderD128',BROOK_GPU_DISPLAY='headless').items():os.environ[key]=value
shutil.copy2(__file__,r/'acceptance-driver.py')
(r/'source.patch').write_bytes(subprocess.check_output(['git','diff']))
for name in ('entry-state','scheduler-start'):
 source=args.artifacts/(name+'-audit.json');v=json.loads(source.read_text());assert v['pass'] and v['sha256']==b.sha(art/'kernel/BROOK.elf');shutil.copy2(source,r/source.name)
owned=[]
try:
 p,s,d,t=b.launch(run,args.instance,art,cpus=8,memory='4G',gpu='venus',composite='cpu',scenario='desktop',on_started=owned.append)
 (r/'session.txt').write_text(s);print('BROOK_VULKAN_CONTEXT_READY',s,t,flush=True)
except BaseException as error:
 # Only the timeout wrapper created above is ours. A boot failure cannot
 # qualify for acceptance; preserve its private disks even if stop is abrupt.
 b.write(r/'launch-failure.json',dict(error=repr(error),orderly_shutdown_proven=False))
 if owned:
  wrapper=owned[0]
  if wrapper.poll() is None:wrapper.terminate()
  wrapper.wait(timeout=30)
  b.write(r/'launch-failure-exit.json',dict(pid=wrapper.pid,exit_code=wrapper.returncode))
 raise
def command(name,argv,expected=0):
 out=r/name;out.mkdir();j=b.TrackedCommand(out,s,argv);j.evidence['request'].update(uid=1000,cwd='/data/home',env={'VK_DRIVER_FILES':control['guestICD'],'XDG_RUNTIME_DIR':'/tmp'});b.write(j.path,j.evidence);end=time.monotonic()+90
 while True:
  try:j.poll()
  except RuntimeError:
   if j.evidence['status'].get('state')!='completed':raise
  v=j.evidence['status']
  if v.get('state')=='completed' and v['guest']['live_tasks']==0 and all(v[n]['eof'] for n in ('stdout','stderr')):break
  if time.monotonic()>end:raise TimeoutError('Resume same tracked command '+name)
  time.sleep(.1)
 assert v['exit']['code']==expected and v['exit']['signal'] is None,v
 for n in ('stdout','stderr'):
  assert not v[n]['gaps'] and not v[n]['dropped_bytes'] and not v[n]['read_error']
  assert len(j.evidence[n].encode())==v['guest'][n+'_bytes']==j.evidence[n+'_offset']
 print('COMMAND_PASS',name,j.evidence['stdout'][-1200:],flush=True);return j.evidence['stdout']
def verify_output(job):
 v=job.evidence['status'];assert job.complete()
 for name in ('stdout','stderr'):
  assert not v[name]['gaps'] and not v[name]['dropped_bytes'] and not v[name]['read_error']
  assert len(job.evidence[name].encode())==v['guest'][name+'_bytes']==job.evidence[name+'_offset']
def snapshot(name):
 result=b.server.capture(s,d,boot_id=boot_id);b.write(r/(name+'-frame.json'),result['data']);shutil.copy2(result['data']['artifact'],r/(name+'.png'));return result['data']
def verify_animation(name,window):
 from PIL import Image,ImageChops
 # Exclude desktop clocks, chrome and pointer positions outside the client.
 x=window['x']+(window['outer_width']-window['width'])//2
 y=window['y']+window['outer_height']-window['height']-(window['outer_width']-window['width'])//2
 bounds=(x+8,y+8,x+window['width']-8,y+window['height']-8)
 first=Image.open(r/(name+'-initial.png')).convert('RGB').crop(bounds)
 second=Image.open(r/(name+'-animated.png')).convert('RGB').crop(bounds)
 changed=sum(pixel!=(0,0,0) for pixel in ImageChops.difference(first,second).getdata())
 b.write(r/(name+'-animation.json'),dict(client_crop=bounds,changed_pixels=changed))
 assert changed>64,'No material client animation observed'
def windows():return b.server.windows(s)['windows']
def wait_window(predicate,deadline=30):
 end=time.monotonic()+deadline
 while time.monotonic()<end:
  j.poll();current=windows();found=next((w for w in current if w['title']=='vkcube' and predicate(w)),None)
  if found:return found
  if j.complete():raise RuntimeError('vkcube exited early: '+j.evidence['stderr'])
  time.sleep(.15)
 raise TimeoutError('window predicate')
def action(w,name):
 value=b.server.window_action(s,w['reference'],name,uuid.uuid4().hex,restore_target=('previous' if w.get('minimized') else 'normal') if name=='restore' else None,wait_for='applied',wait_s=2);b.write(r/(cycle+'-'+name+'.json'),value)
try:
 b.write(r/'boot-stats.json',b.server.stats(s,gpu_details=True))
 renderer=Path(control['virglrenderer'])/'lib/libvirglrenderer.so.1'
 loaded=[]
 for entry in Path('/proc').glob('[0-9]*/exe'):
  try:
   if 'qemu-system' not in entry.resolve(strict=True).name:continue
   for line in (entry.parent/'maps').read_text().splitlines():
    if 'libvirglrenderer.so' in line:loaded.append(line.split()[-1])
  except OSError:continue
 assert loaded and all(Path(path).resolve()==renderer.resolve() for path in loaded)
 b.write(r/'host-renderer.json',dict(loaded=sorted(set(loaded)),expected=str(renderer.resolve()),sha256=b.sha(renderer.resolve())))
 boot_id=b.server.windows(s)['guest_boot_id']
 for iteration in range(2):
  cycle='cycle'+str(iteration+1);out=r/cycle;out.mkdir();started=time.monotonic()
  j=b.TrackedCommand(out,s,[control['vulkanTools']+'/bin/vkcube','--wsi','wayland','--width','1280','--height','720','--c','6000'])
  j.evidence['request'].update(uid=1000,cwd='/data/home',env={'VK_DRIVER_FILES':control['guestICD'],'XDG_RUNTIME_DIR':'/tmp','WAYLAND_DISPLAY':'wayland-0','MESA_VK_WSI_DEBUG':'sw,noshm'});b.write(j.path,j.evidence)
  w=wait_window(lambda w:w['buffer_width']==800 and w['buffer_height']==600);b.write(out/'initial-window.json',w)
  initial=snapshot(cycle+'-initial');time.sleep(.5);second=snapshot(cycle+'-animated');verify_animation(cycle,w)
  x=w['x']+w['outer_width']+1;y=w['y']+w['outer_height']+1
  actions=[{'type':'move','x':x,'y':y},{'type':'button_down'},{'type':'wait','seconds':.2},{'type':'move','x':x+480,'y':y+120},{'type':'wait','seconds':.2},{'type':'button_up'},{'type':'wait','seconds':.5}]
  b.write(out/'resize-input.json',b.server.input_actions(s,actions,frame_id=second['frame_id']))
  w=wait_window(lambda w:w['buffer_width']==1280 and w['buffer_height']==720);b.write(out/'720p-window.json',w);snapshot(cycle+'-720p')
  action(w,'minimize');w=wait_window(lambda w:w['minimized']);b.write(out/'minimized-window.json',w)
  action(w,'restore');w=wait_window(lambda w:not w['minimized'] and w['visible']);snapshot(cycle+'-restored')
  action(w,'maximize');w=wait_window(lambda w:w['maximized'] and w['buffer_width']>1800 and w['buffer_height']>900);b.write(out/'maximized-window.json',w);snapshot(cycle+'-1080p-desktop')
  name='gpu_egl_readback_check.py';argv=[name,'--brook-guest','--mesa',control['mesa'],'--glvnd',control['glvnd'],'--persistent'];code='import sys;sys.argv='+repr(argv)+';\n'+Path('scripts/workloads',name).read_text()
  assert json.loads(command(cycle+'-concurrent-gl',['/nix/profile/bin/python3','-c',code]))['passed']
  action(w,'restore');w=wait_window(lambda w:not w['maximized'] and w['buffer_width']==1280 and w['buffer_height']==720);snapshot(cycle+'-restore-size')
  action(w,'close');end=time.monotonic()+60
  while not j.complete():
   j.poll();assert time.monotonic()<end;time.sleep(.1)
  verify_output(j)
  assert 'Selected GPU 0: Virtio-GPU Venus (Intel(R) UHD Graphics 630' in j.evidence['stderr'] and 'error' not in j.evidence['stderr'].lower(),j.evidence['stderr']
  end=time.monotonic()+15
  while True:
   stats=b.server.stats(s,gpu_details=True,memory_details=True);g=stats['gpu_backing']
   if not any(g[k] for k in ('host_blob_resources','host_aperture_leases','host_aperture_bytes','host_blob_quarantined','quarantined_resources')) and g['allocated_mappings']==g['released_mappings'] and g['allocated_page_bytes']==g['released_page_bytes']:break
   assert time.monotonic()<end,g;time.sleep(.1)
  assert not windows();b.write(out/'after-stats.json',stats);b.write(out/'timing.json',dict(seconds=time.monotonic()-started,comparative_benchmark=False))
  print('VKCUBE_CYCLE_PASS',cycle,flush=True)
 if args.vkmark:
  package=args.vkmark.resolve()
  for iteration in range(2):
   cycle='vkmark'+str(iteration+1);out=r/cycle;out.mkdir();started=time.monotonic()
   argv=[str(package/'bin/vkmark'),'--winsys','wayland','--present-mode','fifo','--size','1280x720','--benchmark','vertex:duration=15','--benchmark','texture:duration=15']
   j=b.TrackedCommand(out,s,argv);j.evidence['request'].update(uid=1000,cwd='/data/home',env={'VK_DRIVER_FILES':control['guestICD'],'XDG_RUNTIME_DIR':'/tmp','WAYLAND_DISPLAY':'wayland-0','MESA_VK_WSI_DEBUG':'sw,noshm'});b.write(j.path,j.evidence)
   end=time.monotonic()+90;first=None
   while time.monotonic()<end:
    j.poll();current=windows();first=next((w for w in current if 'vkmark' in w['title'].lower()),None)
    if first:break
    assert not j.complete(),j.evidence;time.sleep(.2)
   assert first,'vkmark did not map a window';b.write(out/'first-window.json',first)
   initial=snapshot(cycle+'-initial');time.sleep(.5);second=snapshot(cycle+'-animated');verify_animation(cycle,first)
   name='gpu_egl_readback_check.py';glargv=[name,'--brook-guest','--mesa',control['mesa'],'--glvnd',control['glvnd'],'--persistent'];code='import sys;sys.argv='+repr(glargv)+';\n'+Path('scripts/workloads',name).read_text()
   assert json.loads(command(cycle+'-concurrent-gl',['/nix/profile/bin/python3','-c',code]))['passed']
   while not j.complete():
    j.poll();assert time.monotonic()<end,j.evidence;time.sleep(.1)
   verify_output(j)
   combined=j.evidence['stdout']+j.evidence['stderr'];assert 'Virtio-GPU Venus (Intel(R) UHD Graphics 630' in combined and 'Error' not in combined,combined
   fps={}
   for scene in ('vertex','texture'):
    matches=re.findall(r'\['+scene+r'\][^\n]*FPS:\s*([0-9]+)',combined)
    assert len(matches)==1 and int(matches[0])>0,(scene,combined)
    fps[scene]=int(matches[0])
   b.write(out/'scene-fps.json',dict(fps=fps,comparative_benchmark=False))
   end=time.monotonic()+15
   while True:
    stats=b.server.stats(s,gpu_details=True,memory_details=True);g=stats['gpu_backing']
    if not any(g[k] for k in ('host_blob_resources','host_aperture_leases','host_aperture_bytes','host_blob_quarantined','quarantined_resources')) and g['allocated_mappings']==g['released_mappings'] and g['allocated_page_bytes']==g['released_page_bytes']:break
    assert time.monotonic()<end,g;time.sleep(.1)
   assert not windows();b.write(out/'after-stats.json',stats);b.write(out/'timing.json',dict(seconds=time.monotonic()-started,comparative_benchmark=False));print('VKMARK_CYCLE_PASS',cycle,combined,flush=True)
 heap=b.server.guest(d,'heap check');b.write(r/'heap-check.json',heap);assert heap['reply']=='heap integrity: OK'

finally:
 try:
  b.write(r/'final-stats.json',b.server.stats(s,audio_details=True,gpu_details=True,memory_details=True));b.write(r/'final-observation.json',b.server.observe(s,include=['windows','processes']))
 finally:
  request=dict(session=s,action='shutdown',request_id=uuid.uuid4().hex,wait_s=10);b.write(r/'shutdown-request.json',request)
  try:
   b.write(r/'shutdown-result.json',b.server.lifecycle_operation(**request));p.wait(timeout=120)
  finally:
   if p.poll() is None:
    b.write(r/'forced-stop.json',dict(reason='Owned private launcher did not finish checked shutdown',pid=p.pid))
    p.terminate();p.wait(timeout=30)
   b.wait_existing_vm(d)
  b.wait_launcher(p,r,s)
  try:b.check_stopped_serial(d,r)
  finally:b.write(r/'after-fsck.json',b.offline_fsck(run,'after'))
  status=b.server.lifecycle_operation(**request);b.write(r/'shutdown-result.json',status);assert status['orderly_shutdown_completed'] and status['qemu_exit_code']==0 and not (r/'forced-stop.json').exists()
  text=(r/'guest-final-serial.log').read_text();assert text.count('CPU: PAT WC slot4 ready value=0x7040100070406')==8;assert 'write-combining disabled' not in text
  errors=[line for line in (text+(run/'launch.log').read_text(errors='replace')).splitlines() if re.search(r'context error|failed to dispatch|virtio_gpu_virgl_process_cmd.*error|IOV data size exceeds|failed to open libvulkan|invalid.*completion|control queue.*poison',line,re.I)]
  b.write(r/'renderer-errors.json',dict(passed=not errors,errors=errors));assert not errors,errors
  print('BROOK_PAT_CHECKED_STOP',s,flush=True)

b.write(r/'acceptance-complete.json',dict(passed=True,session=s,window_cycles=2,vkmark_cycles=2 if args.vkmark else 0,presentation='native Mesa Wayland WSI with hardware Venus rendering and CPU staging',checked_shutdown=True,offline_fsck_passed=True))
